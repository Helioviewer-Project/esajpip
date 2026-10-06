// Live recovery checks. Starts its own short-timeout server; never touches
// an existing server: node check_recovery.mjs wasm server-binary image-directory image [other-image]
import assert from "node:assert/strict";
import { readFile, mkdtemp, writeFile, rm } from "node:fs/promises";
import { createServer as createHttpServer } from "node:http";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { createServer, createConnection } from "node:net";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { setTimeout as delay } from "node:timers/promises";
import { JpipChannel } from "../../client/js/jpip_channel.mjs";
import { JpipSource } from "../../client/js/jpip_source.mjs";
import { Worker } from "./worker.mjs";

const [wasmPath, binary, directory, image, otherImage = image] = process.argv.slice(2);
if (!image) throw new Error("usage: check_recovery.mjs wasm server-binary image-directory image [other-image]");
const wasm = await readFile(wasmPath);
globalThis.Worker = Worker;
const temporary = await mkdtemp(join(tmpdir(), "esajpip-recovery-"));
const reservation = createServer();
await new Promise(resolve => reservation.listen(0, "127.0.0.1", resolve));
const port = reservation.address().port;
await new Promise(resolve => reservation.close(resolve));
const server = `http://127.0.0.1:${port}`;
await writeFile(join(temporary, "server.ini"), `[listen]
port=${port}
address=127.0.0.1
[jpip]
image_directory=${resolve(directory)}
chunk_size=131072
[connections]
limit=128
initial_timeout=3
timeout=1
[channels]
limit=256
[logging]
directory=
file_enabled=false
requests=false
`);

let processServer;
async function start() {
    processServer = spawn(resolve(binary), [], { cwd: temporary, stdio: ["ignore", "ignore", "pipe"] });
    let log = "";
    processServer.stderr.on("data", bytes => { log += bytes; });
    for (let attempt = 0; attempt < 100; attempt++) {
        if (processServer.exitCode !== null) throw new Error(`server exited: ${log}`);
        const connected = await new Promise(resolve => {
            const socket = createConnection({ port, host: "127.0.0.1" });
            socket.once("connect", () => { socket.destroy(); resolve(true); });
            socket.once("error", () => resolve(false));
        });
        if (connected) return;
        await delay(20);
    }
    throw new Error(`server did not start: ${log}`);
}
async function stop() {
    if (!processServer || processServer.exitCode !== null) return;
    const exited = once(processServer, "exit");
    processServer.kill("SIGTERM");
    await exited;
}

const realFetch = globalThis.fetch;
let requests = [];
globalThis.fetch = (url, options) => {
    const request = new URL(url);
    assert.ok(new TextEncoder().encode(`GET ${request.pathname}${request.search} HTTP/1.1`).length <= 2048,
              "recovery exceeded the request-line limit");
    requests.push(request);
    return realFetch(url, options);
};
const trackedFetch = globalThis.fetch;
const sources = [];
async function open() {
    const source = await JpipChannel.open(wasm, server, image);
    sources.push(source);
    return source;
}
function declarations() { return requests.filter(url => url.searchParams.has("model")); }
function cnews() { return requests.filter(url => url.searchParams.has("cnew")); }

try {
    await start();
    const reference = await open();
    const expected = await reference.frame(0, { fit: [2048, 2048] });
    const source = await open();
    const preview = await source.frame(0, { fit: [2048, 2048], layers: 1 });
    const metadata = source.xml(0);
    const palette = source.palette(0);
    const otherFrames = [];
    for (let index = 1; index < Math.min(source.frames, 4); index++)
        otherFrames.push(await source.frame(index, { fit: [512, 512], layers: 1 }));

    // Expiry preserves preview status and pixels, then refinement resumes with
    // exact prefixes on a new channel rather than starting the image over.
    await delay(2100);
    requests = [];
    assert.deepEqual((await source.frame(0, { fit: [2048, 2048], layers: 1 })).pixels, preview.pixels);
    assert.equal(requests.length, 0, "cached preview made a request after expiry");
    const recovered = await source.frame(0, { fit: [2048, 2048] });
    assert.deepEqual(recovered.pixels, expected.pixels, "expiry recovery changed pixels");
    assert.equal(cnews().length, 1, "expiry did not create exactly one replacement");
    assert.ok(declarations().length > 0);
    assert.ok(declarations().every(url => url.searchParams.get("stream") === String(source.frames) &&
                                         url.searchParams.get("layers") === "0"));
    assert.ok(!declarations().some(url => /(?:^|,)M0:/.test(url.searchParams.get("model"))));
    assert.equal(source.xml(0), metadata);
    assert.deepEqual(source.palette(0), palette);
    const afterRecovery = requests.length;
    for (const frame of otherFrames)
        assert.deepEqual(await source.frame(frame.index, { fit: [512, 512], layers: 1 }), frame);
    assert.equal(requests.length, afterRecovery, "other frames lost their retained quality");
    console.log(`Expiry: ${declarations().length} bounded cache declarations; preview and metadata retained`);

    // A restart forgets every channel, but the immutable target still matches.
    const restarted = await open();
    await restarted.frame(0, { fit: [2048, 2048], layers: 1 });
    await stop();
    await start();
    requests = [];
    assert.deepEqual((await restarted.frame(0, { fit: [2048, 2048] })).pixels, expected.pixels);
    assert.equal(cnews().length, 1);
    console.log("Restart: retained cache restored and refinement matches uninterrupted transfer");

    // Lose a response body after the server counted its bytes. The client did
    // not apply any of that body and must discard the server's old cache model.
    const interrupted = await open();
    await interrupted.frame(0, { fit: [2048, 2048], layers: 1 });
    requests = [];
    let lost = false;
    globalThis.fetch = async (url, options) => {
        const response = await trackedFetch(url, options);
        if (!lost && !new URL(url).searchParams.has("cclose")) {
            lost = true;
            await response.arrayBuffer();
            return new Response(new ReadableStream({
                start(controller) { controller.error(new TypeError("lost body")); },
            }), { status: response.status, headers: response.headers });
        }
        return response;
    };
    assert.deepEqual((await interrupted.frame(0, { fit: [2048, 2048] })).pixels, expected.pixels);
    assert.equal(cnews().length, 1);
    globalThis.fetch = trackedFetch;
    console.log("Interrupted body: new channel resumes from bytes actually retained");

    // Both the original and resumed request fail: one recovery, no loop.
    // Cache hits still work; a later call can recover when service returns.
    const bounded = await open();
    const saved = await bounded.frame(0, { fit: [2048, 2048], layers: 1 });
    requests = [];
    let failures = 0;
    globalThis.fetch = async (url, options) => {
        const request = new URL(url);
        if (!request.searchParams.has("model") && !request.searchParams.has("cclose")) {
            failures++;
            return new Response("JPIP channel does not exist", { status: 503 });
        }
        return trackedFetch(url, options);
    };
    await assert.rejects(bounded.frame(0, { fit: [2048, 2048] }), /503/);
    assert.equal(failures, 2);
    assert.equal(cnews().length, 1);
    const count = requests.length;
    assert.deepEqual((await bounded.frame(0, { fit: [2048, 2048], layers: 1 })).pixels, saved.pixels);
    assert.equal(requests.length, count);
    globalThis.fetch = trackedFetch;
    assert.deepEqual((await bounded.frame(0, { fit: [2048, 2048] })).pixels, expected.pixels);

    // A malformed response after a valid preview retires the cache, not just
    // the request. Already returned JavaScript pixels remain caller-owned.
    const refused = await open();
    const retained = await refused.frame(0, { fit: [2048, 2048], layers: 1 });
    const retainedPixels = retained.pixels.slice();
    globalThis.fetch = async () => new Response(new Uint8Array([0]), { status: 200 });
    let refusal;
    await assert.rejects(refused.frame(0, { fit: [2048, 2048] }), error => { refusal = error; return true; });
    for (const read of [() => refused.cached(0), () => refused.xml(0), () => refused.palette(0)])
        assert.throws(read, error => error === refusal);
    await assert.rejects(refused.frame(0, { fit: [2048, 2048], layers: 1 }));
    assert.equal(refused.frames, 0);
    assert.deepEqual(retained.pixels, retainedPixels);
    globalThis.fetch = trackedFetch;

    // Busy, invalid, unavailable and malformed replies must never reopen.
    for (const [code, body] of [[503, "JPIP channel is busy"], [503, "JPIP channel limit has been reached"],
                               [400, "bad request"], [404, "not found"], [500, "failure"],
                               [200, new Uint8Array([0])]]) {
        const terminal = await open();
        requests = [];
        let calls = 0;
        globalThis.fetch = async () => { calls++; return new Response(body, { status: code }); };
        await assert.rejects(terminal.frame(0, { fit: [2048, 2048] }));
        assert.equal(calls, 1);
        await assert.rejects(terminal.frame(0, { fit: [2048, 2048] }));
        assert.equal(calls, 1, "terminal error retried");
        globalThis.fetch = trackedFetch;
    }

    // A close requested during a failed transport prevents reopening.
    const closing = await open();
    let release;
    const pending = new Promise(resolve => { release = resolve; });
    let entered;
    const waiting = new Promise(resolve => { entered = resolve; });
    requests = [];
    globalThis.fetch = async (url, options) => {
        if (!new URL(url).searchParams.has("cclose")) {
            entered();
            await pending;
            throw new TypeError("disconnected");
        }
        return trackedFetch(url, options);
    };
    const frame = closing.frame(0, { fit: [2048, 2048] });
    const rejected = assert.rejects(frame, /channel is closed/);
    await waiting;
    const closed = closing.close();
    release();
    await rejected;
    await closed;
    assert.equal(cnews().length, 0, "close reopened a channel");
    globalThis.fetch = trackedFetch;
    console.log("Bounded failure, terminal errors and close checks passed");

    // Observe real worker HTTP requests through a local forwarding server.
    // Fetch substitution on the main thread does not affect a worker's fetch.
    const workerRequests = [];
    const workerCids = [];
    const moduleRequests = new Map();
    let heldRestore = null;
    const workerHost = createHttpServer(async (request, response) => {
        const url = new URL(request.url, server);
        if (url.pathname.endsWith(".wasm")) {
            const count = (moduleRequests.get(url.pathname) ?? 0) + 1;
            moduleRequests.set(url.pathname, count);
            if (count === 1 && url.pathname === "/unavailable.wasm") {
                response.writeHead(503);
                response.end("temporary download failure");
            } else {
                response.end(count === 1 && url.pathname === "/invalid.wasm" ? new Uint8Array([0]) : wasm);
            }
            return;
        }
        workerRequests.push(url);
        try {
            const upstream = await realFetch(server + request.url);
            const cid = /(?:^|,)cid=([^,]+)/.exec(upstream.headers.get("JPIP-cnew") ?? "");
            if (cid) workerCids.push(cid[1]);
            const body = new Uint8Array(await upstream.arrayBuffer());
            if (heldRestore && url.searchParams.has("cnew") && url.searchParams.has("model")) {
                heldRestore.entered();
                await heldRestore.release;
            }
            response.writeHead(upstream.status, Object.fromEntries(upstream.headers));
            response.end(body);
        } catch (error) {
            response.writeHead(500);
            response.end(String(error));
        }
    });
    await new Promise(resolve => workerHost.listen(0, "127.0.0.1", resolve));
    const workerServer = `http://127.0.0.1:${workerHost.address().port}`;
    const workerSources = [];
    async function openWorker(name = image, module = "client.wasm") {
        const source = await JpipSource.open({ wasm: `${workerServer}/${module}`, server: workerServer, image: name });
        workerSources.push(source);
        return source;
    }
    try {
        // Failed promises are removed, successful compilation is still shared
        // by concurrent opens. Exercise HTTP and compilation failures separately.
        for (const [module, error] of [["unavailable.wasm", /503/], ["invalid.wasm", WebAssembly.CompileError]]) {
            await assert.rejects(openWorker(image, module), error);
            const opened = await Promise.all([openWorker(image, module), openWorker(image, module)]);
            assert.equal(moduleRequests.get(`/${module}`), 2, "module failure poisoned retry or compiled twice");
            await Promise.all(opened.map(source => source.close()));
        }
        const otherReference = await JpipChannel.open(wasm, server, otherImage);
        sources.push(otherReference);
        const otherExpected = await otherReference.frame(0, { fit: [2048, 2048] });
        for (const event of ["expiry", "restart"]) {
            const [first, second] = await Promise.all([openWorker(), openWorker(otherImage)]);
            const previews = await Promise.all([
                first.frame(0, { fit: [1024, 1024], layers: 1 }),
                second.frame(0, { fit: [2048, 2048], layers: 1 }),
            ]);
            const xml = await Promise.all([first.xml(0), second.xml(0)]);
            if (event === "expiry") await delay(2100);
            else { await stop(); await start(); }
            workerRequests.length = 0;
            workerCids.length = 0;
            assert.deepEqual((await first.frame(0, { fit: [1024, 1024], layers: 1 })).pixels, previews[0].pixels);
            assert.deepEqual((await second.frame(0, { fit: [2048, 2048], layers: 1 })).pixels, previews[1].pixels);
            assert.equal(workerRequests.length, 0, "worker preview lost its cache");
            const refined = await Promise.all([
                first.frame(0, { fit: [2048, 2048] }), second.frame(0, { fit: [2048, 2048] }),
            ]);
            assert.deepEqual(refined[0].pixels, expected.pixels);
            assert.deepEqual(refined[1].pixels, otherExpected.pixels);
            const opened = workerRequests.filter(url => url.searchParams.has("cnew"));
            assert.equal(opened.length, 2, "worker sources did not recover independently");
            assert.equal(new Set(workerCids).size, 2, "worker sources shared a channel identity");
            assert.ok(workerRequests.filter(url => url.searchParams.has("model") &&
                url.searchParams.has("cid")).every(url => workerCids.includes(url.searchParams.get("cid"))));
            assert.deepEqual(await Promise.all([first.xml(0), second.xml(0)]), xml);
            await first.close();
            const count = workerRequests.length;
            assert.deepEqual((await second.frame(0, { fit: [2048, 2048] })).pixels, otherExpected.pixels);
            assert.equal(workerRequests.length, count, "closing one movie affected another");
            await second.close();
        }

        const closing = await openWorker();
        await closing.frame(0, { fit: [2048, 2048], layers: 1 });
        await stop();
        await start();
        let release, entered;
        const waiting = new Promise(resolve => { entered = resolve; });
        heldRestore = { entered, release: new Promise(resolve => { release = resolve; }), resume: () => release() };
        workerRequests.length = 0;
        const rejected = assert.rejects(closing.frame(0, { fit: [2048, 2048] }), /channel is closed/);
        await waiting;
        const closed = closing.close();
        await rejected;
        await closed;
        release();
        heldRestore = null;
        assert.equal(workerRequests.filter(url => url.searchParams.has("cnew")).length, 1);
        assert.equal(workerRequests.filter(url => !url.searchParams.has("model") &&
            !url.searchParams.has("cclose")).length, 1, "worker resumed data delivery after close");
        console.log("Worker: module retry, independent movies, expiry, restart and close during recovery passed");
    } finally {
        heldRestore?.resume();
        for (const source of workerSources) await source.close().catch(() => {});
        workerHost.closeAllConnections();
        await new Promise(resolve => workerHost.close(resolve));
    }

    // A reverse proxy can give a short file a long public URL, without relying
    // on filesystem path limits. Both original and channel paths need batching
    // within the actual space remaining on their request lines.
    const prefix = "/" + "x".repeat(1850);
    const proxy = createHttpServer(async (request, response) => {
        try {
            const upstream = await realFetch(server + request.url.slice(prefix.length));
            response.writeHead(upstream.status, Object.fromEntries(upstream.headers));
            response.end(new Uint8Array(await upstream.arrayBuffer()));
        } catch (error) {
            response.writeHead(500);
            response.end(String(error));
        }
    });
    await new Promise(resolve => proxy.listen(0, "127.0.0.1", resolve));
    try {
        const long = await JpipChannel.open(wasm,
            `http://127.0.0.1:${proxy.address().port}${prefix}`, image);
        sources.push(long);
        await long.frame(0, { fit: [2048, 2048], layers: 1 });
        await stop();
        await start();
        requests = [];
        assert.deepEqual((await long.frame(0, { fit: [2048, 2048] })).pixels, expected.pixels);
        assert.ok(cnews()[0].searchParams.get("model").length < 128);
        await long.close();
        console.log("Long URL: cache restoration fits batches smaller than 128 bytes");
    } finally {
        proxy.closeAllConnections();
        await new Promise(resolve => proxy.close(resolve));
    }
} finally {
    globalThis.fetch = realFetch;
    for (const source of sources) await source.close().catch(() => {});
    await stop();
    await rm(temporary, { recursive: true, force: true });
}
