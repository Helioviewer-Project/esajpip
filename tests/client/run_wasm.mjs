// Automated WASM checks against a private local server and checked-in fixtures.
// node run_wasm.mjs wasm server-binary fixture-writer repository
import assert from "node:assert/strict";
import { copyFile, mkdtemp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { createServer as httpServer } from "node:http";
import { createConnection, createServer } from "node:net";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { fileURLToPath } from "node:url";
import { setTimeout as delay } from "node:timers/promises";
import { JpipChannel } from "../../client/js/jpip_channel.mjs";
import { JpipSource } from "../../client/js/jpip_source.mjs";
import { Worker } from "./worker.mjs";
import { checkTransport } from "./check_transport.mjs";
import { checkBoundary } from "./check_boundary.mjs";
import { checkHost } from "./check_host.mjs";

const [wasmPath, binary, writer, repository] = process.argv.slice(2);
if (!repository) throw new Error("usage: run_wasm.mjs wasm server-binary fixture-writer repository");
const wasm = await readFile(wasmPath);
const temporary = await mkdtemp(join(tmpdir(), "esajpip-wasm-"));
const images = join(temporary, "images");
const fixtures = ["synthetic_rgb_129x129_CPRL_SOP_EPH.jp2", "solo_fsi174_509x513_PCRL.jp2"];

async function run(command, args) {
    const child = spawn(command, args, { stdio: "inherit" });
    const [code, signal] = await once(child, "exit");
    assert.equal(code, 0, `${command} failed (${signal ?? code})`);
}
let moduleRequests = 0, canceledBodies = 0;
const host = httpServer((request, response) => {
    if (request.url === "/missing.wasm") {
        response.writeHead(404);
        response.write("module missing"); // Keep the body open until the client cancels it.
        response.on("close", () => { canceledBodies++; });
    } else {
        moduleRequests++;
        response.end(wasm);
    }
});
let processServer;
try {
    await mkdir(images);
    for (const name of fixtures)
        await copyFile(join(repository, "tests/transcode/fixtures/kakadu", name), join(images, name));
    const escapedImage = 'space #?%20&+"é.jp2';
    await copyFile(join(images, fixtures[0]), join(images, escapedImage));
    await copyFile(join(repository, "tests/merge/fixtures/expected/merged.jpx"), join(images, "movie.jpx"));
    const reservation = createServer();
    await new Promise((resolve, reject) => {
        reservation.once("error", reject);
        reservation.listen(0, "127.0.0.1", resolve);
    });
    const port = reservation.address().port;
    await new Promise(resolve => reservation.close(resolve));
    const server = `http://127.0.0.1:${port}`;
    await writeFile(join(temporary, "server.ini"), `[listen]\nport=${port}\naddress=127.0.0.1\n[jpip]\nimage_directory=${images}\nchunk_size=131072\n[connections]\nlimit=128\ninitial_timeout=3\ntimeout=60\n[channels]\nlimit=256\n[logging]\ndirectory=\nfile_enabled=false\nrequests=false\n`);
    await run(resolve(writer), ["--write-fixtures", images]);
    processServer = spawn(resolve(binary), [], { cwd: temporary, stdio: ["ignore", "ignore", "pipe"] });
    let log = "";
    processServer.stderr.on("data", bytes => { log += bytes; });
    let connected = false;
    for (let attempt = 0; attempt < 100 && !connected; attempt++) {
        if (processServer.exitCode !== null) throw new Error(`server exited: ${log}`);
        connected = await new Promise(resolve => {
            const socket = createConnection({ port, host: "127.0.0.1" });
            socket.once("connect", () => { socket.destroy(); resolve(true); });
            socket.once("error", () => resolve(false));
        });
        if (!connected) await delay(20);
    }
    assert.ok(connected, `server did not start: ${log}`);
    await new Promise((resolve, reject) => {
        host.once("error", reject);
        host.listen(0, "127.0.0.1", resolve);
    });

    // Exercise an actual worker import failure during open(), including cleanup.
    globalThis.Worker = class extends Worker {
        constructor() { super(new URL("./missing-worker.mjs", import.meta.url)); }
    };
    await assert.rejects(JpipSource.open({
        wasm: `http://127.0.0.1:${host.address().port}/client.wasm`, server, image: fixtures[0],
    }), { name: "Error", message: "the worker failed" });
    globalThis.Worker = Worker;

    const wasmURL = `http://127.0.0.1:${host.address().port}/client.wasm`;
    const missingURL = new URL("missing.wasm", wasmURL).href;
    for (const open of [() => JpipChannel.open(missingURL, server, fixtures[0], { timeout: 250 }),
                       () => JpipSource.open({ wasm: missingURL, server, image: fixtures[0] })])
        await assert.rejects(open(), { name: "Error", message: `404 ${missingURL}` });
    for (let i = 0; i < 50 && canceledBodies < 2; i++) await delay(10);
    assert.equal(canceledBodies, 2, "failed module fetch bodies were not canceled");
    globalThis.location = { href: new URL("page/index.html", wasmURL).href };
    const downloads = moduleRequests;
    try {
        for (const url of ["./alias.wasm", new URL("page/alias.wasm", wasmURL).href]) {
            const source = await JpipSource.open({ wasm: url, server, image: fixtures[0] });
            await source.close();
        }
        assert.equal(moduleRequests - downloads, 1, "equivalent module URLs downloaded twice");
    } finally { delete globalThis.location; }
    await checkBoundary(wasm, wasmURL, server, fixtures[0], repository);
    await checkHost(wasm, wasmURL, server, fixtures[0]);

    // Occupy the low heap with live allocations. Merely growing memory leaves
    // small free chunks below 2 GiB that can hide signed-pointer mistakes.
    const instantiate = WebAssembly.instantiate;
    let wasmExports;
    const reserved = [], responses = [];
    WebAssembly.instantiate = async (...args) => {
        const instance = await instantiate(...args);
        wasmExports = instance.exports;
        return { exports: {
            ...wasmExports,
            _initialize() {
                wasmExports._initialize();
                for (let i = 0; i < 2; i++) {
                    const at = wasmExports.hvc_wasm_alloc(1024 ** 3) >>> 0;
                    assert.notEqual(at, 0);
                    reserved.push(at);
                }
                let at;
                do {
                    at = wasmExports.hvc_wasm_alloc(1) >>> 0;
                    assert.notEqual(at, 0);
                    reserved.push(at);
                } while (at < 2 ** 31);
            },
            hvc_wasm_alloc(size) {
                const at = wasmExports.hvc_wasm_alloc(size);
                responses.push(at >>> 0);
                return at;
            },
        } };
    };
    let channel, expected;
    try {
        channel = await JpipChannel.open(wasm, server, fixtures[0]);
    } finally {
        WebAssembly.instantiate = instantiate;
    }
    try {
        const frame = await channel.frame(0);
        assert.equal(frame.width, 129);
        assert.equal(frame.height, 129);
        assert.equal(frame.components, 3);
        assert.ok(responses.length >= 2 && responses.every(at => at >= 2 ** 31),
                  "response bodies did not exercise high addresses");
        assert.ok((wasmExports.hvc_wasm_pixels() >>> 0) >= 2 ** 31,
                  "pixels did not exercise a high address");
        assert.equal(typeof channel.xml(0), "string");
        assert.ok((wasmExports.hvc_wasm_xml() >>> 0) >= 2 ** 31,
                  "XML did not exercise a high address");
        // Independent reference: the fixture's lossless source formula.
        for (let y = 0; y < 129; y++)
            for (let x = 0; x < 129; x++) {
                const at = (y * 129 + x) * 3;
                assert.equal(frame.pixels[at], (x + 3 * y) % 256);
                assert.equal(frame.pixels[at + 1], (5 * x + y) % 256);
                assert.equal(frame.pixels[at + 2], x ^ y);
            }
        expected = frame.pixels;
        channel.palette(0);
    } finally {
        await channel.close();
        for (const at of reserved) wasmExports.hvc_wasm_free(at);
    }
    const mapped = await JpipChannel.open(wasm, server, "shared-palette.jpx");
    try {
        const reversed = mapped.palette(0), direct = mapped.palette(1);
        assert.equal(reversed.channels, 3);
        assert.equal(direct.channels, 3);
        assert.deepEqual(reversed.table.slice(15, 18), new Uint8Array([2, 250, 5]));
        assert.deepEqual(direct.table.slice(15, 18), new Uint8Array([5, 250, 2]));
        assert.deepEqual(mapped.palette(0), reversed, "shared codestream overwrote the first layer's palette");
    } finally { await mapped.close(); }

    // Nine live instances exercise allocation growth beyond the engine's
    // small-instance reservation case. Growth is independent of block rounding.
    const heaps = [], channels = [];
    WebAssembly.instantiate = async (...args) => {
        const instance = await instantiate(...args);
        heaps.push(instance.exports);
        return instance;
    };
    try {
        for (let i = 0; i < 9; i++)
            channels.push(await JpipChannel.open(wasm, server, fixtures[0]));
    } finally {
        WebAssembly.instantiate = instantiate;
    }
    try {
        for (const heap of heaps) {
            const blocks = [];
            let growths = 0;
            try {
                for (let i = 0; i < 1024; i++) {
                    const before = heap.memory.buffer.byteLength;
                    const at = heap.hvc_wasm_alloc(32769) >>> 0;
                    assert.notEqual(at, 0);
                    if (blocks.length)
                        assert.ok(at - blocks.at(-1) <= 32769 + 32,
                                  "allocator rounded an individual block to a power of two");
                    blocks.push(at);
                    if (heap.memory.buffer.byteLength !== before) growths++;
                }
                assert.ok(growths <= 64, `32 MiB in small blocks required ${growths} growths`);
            } finally {
                for (const at of blocks) heap.hvc_wasm_free(at);
            }
        }
    } finally {
        for (const channel of channels) await channel.close();
    }

    const source = await JpipSource.open({
        wasm: `http://127.0.0.1:${host.address().port}/client.wasm`, server, image: fixtures[0],
    });
    try {
        assert.deepEqual((await source.frame(0)).pixels, expected, "worker changed known pixels");
    } finally {
        await source.close();
    }
    console.log("High addresses, independent pixels and worker load failure passed");

    const escaped = await JpipChannel.open(wasm, server, escapedImage);
    try { assert.deepEqual((await escaped.frame(0)).pixels, expected); }
    finally { await escaped.close(); }
    // Distribution notices include the additional copyrights in HTJ2K sources.
    const notices = await readFile(join(dirname(resolve(wasmPath)), "OpenJPEG-NOTICES.txt"), "utf8");
    for (const name of ["Aous Naman", "Kakadu Software", "University of New South Wales"])
        assert.ok(notices.includes(name));
    assert.ok((await readFile(join(dirname(resolve(wasmPath)), "dlmalloc-LICENSE.txt"), "utf8")).includes("Permission"));
    await checkTransport(wasm, server, fixtures[0]);

    for (const image of [...fixtures, "movie.jpx"])
        await run(process.execPath, [fileURLToPath(new URL("./check.mjs", import.meta.url)), wasmPath, server, image]);
    await run(process.execPath, [fileURLToPath(new URL("./check_layers.mjs", import.meta.url)), wasmPath, server]);
    await run(process.execPath, [fileURLToPath(new URL("./check_recovery.mjs", import.meta.url)),
        wasmPath, resolve(binary), images, "movie.jpx", fixtures[1]]);
} finally {
    host.closeAllConnections();
    if (host.listening) await new Promise(resolve => host.close(resolve));
    if (processServer && processServer.exitCode === null) {
        const exited = once(processServer, "exit");
        processServer.kill("SIGTERM");
        await exited;
    }
    await rm(temporary, { recursive: true, force: true });
}
