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
const host = httpServer((request, response) => response.end(wasm));
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

    // Force new heap allocations above 2 GiB without filling that address space.
    // This checks the application views of response bodies, XML and decoded pixels.
    const instantiate = WebAssembly.instantiate;
    let wasmExports;
    WebAssembly.instantiate = async (...args) => {
        const instance = await instantiate(...args);
        wasmExports = instance.exports;
        const memory = wasmExports.memory;
        memory.grow(32768 - memory.buffer.byteLength / 65536);
        return instance;
    };
    let channel, expected;
    try {
        channel = await JpipChannel.open(wasm, server, fixtures[0]);
    } finally {
        WebAssembly.instantiate = instantiate;
    }
    try {
        const allocationSize = 1024 * 1024;
        const before = wasmExports.memory.buffer.byteLength;
        const at = wasmExports.hvc_wasm_alloc(allocationSize) >>> 0;
        assert.ok(at >= 2 ** 31, "allocation did not exercise a high address");
        assert.ok(wasmExports.memory.buffer.byteLength - before <= allocationSize + 65536,
                  "allocator rounded a 1 MiB block beyond its page-aligned requirement");
        wasmExports.hvc_wasm_free(at);
        const frame = await channel.frame(0);
        assert.equal(frame.width, 129);
        assert.equal(frame.height, 129);
        assert.equal(frame.components, 3);
        // Independent reference: the fixture's lossless source formula.
        for (let y = 0; y < 129; y++)
            for (let x = 0; x < 129; x++) {
                const at = (y * 129 + x) * 3;
                assert.equal(frame.pixels[at], (x + 3 * y) % 256);
                assert.equal(frame.pixels[at + 1], (5 * x + y) % 256);
                assert.equal(frame.pixels[at + 2], x ^ y);
            }
        expected = frame.pixels;
        assert.equal(typeof channel.xml(0), "string");
        channel.palette(0);
    } finally {
        await channel.close();
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
