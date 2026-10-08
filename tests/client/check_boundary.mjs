// Real module ABI and failure paths, with an injected trap at the host boundary.
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { JpipChannel } from "../../client/js/jpip_channel.mjs";
import { JpipSource } from "../../client/js/jpip_source.mjs";

export async function checkBoundary(wasm, wasmURL, server, image, repository) {
    const module = await WebAssembly.compile(wasm);
    for (const invalid of [module, wasm, {}])
        await assert.rejects(JpipSource.open({ wasm: invalid, server, image }),
                             { name: "TypeError", message: "wasm must be a URL or URL string" });
    const functions = ["_initialize", "hvc_wasm_alloc", "hvc_wasm_free", "hvc_wasm_response",
        "hvc_wasm_restore_response", "hvc_wasm_progress", "hvc_wasm_new_channel", "hvc_wasm_cancel_request", "hvc_wasm_model", "hvc_wasm_model_next",
        "hvc_wasm_frames", "hvc_wasm_codestreams", "hvc_wasm_xml_size", "hvc_wasm_xml",
        "hvc_wasm_palette", "hvc_wasm_palette_channels", "hvc_wasm_palette_table", "hvc_wasm_view",
        "hvc_wasm_decode", "hvc_wasm_pixels", "hvc_wasm_width", "hvc_wasm_height",
        "hvc_wasm_components", "hvc_wasm_error", "hvc_wasm_reset"];
    assert.deepEqual(WebAssembly.Module.exports(module).sort((a, b) => a.name.localeCompare(b.name)),
        [...functions.map(name => ({ name, kind: "function" })), { name: "memory", kind: "memory" }]
            .sort((a, b) => a.name.localeCompare(b.name)));
    const imports = ["fd_close", "environ_get", "environ_sizes_get", "fd_fdstat_get", "fd_seek",
        "fd_write", "proc_exit"];
    assert.deepEqual(WebAssembly.Module.imports(module).sort((a, b) => a.name.localeCompare(b.name)),
        imports.map(name => ({ module: "wasi_snapshot_preview1", name, kind: "function" }))
            .sort((a, b) => a.name.localeCompare(b.name)));
    for (const [from, name] of [["wasi_snapshot_preview1", "unexpected"],
                               ["other_namespace", "fd_write"], ["wasi_snapshot_preview1", "toString"]]) {
        const text = value => [...new TextEncoder().encode(value)];
        const payload = [1, from.length, ...text(from), name.length, ...text(name), 0, 0];
        const bytes = new Uint8Array([0,97,115,109,1,0,0,0, 1,4,1,96,0,0, 2,payload.length,...payload]);
        await assert.rejects(JpipChannel.open(bytes, server, image), /unsupported WebAssembly import/);
    }

    const instantiate = WebAssembly.instantiate;
    let exports, initialized = 0, trap = false, staged = null, freed = null, decodeFailures = 0, unsignedFrames = false, resolutions = null, tailError = null;
    WebAssembly.instantiate = async (...args) => {
        const instance = await instantiate(...args);
        exports = instance.exports;
        return { exports: {
            ...exports,
            _initialize() { initialized++; return exports._initialize(); },
            hvc_wasm_frames() { return unsignedFrames ? -1 : exports.hvc_wasm_frames(); },
            hvc_wasm_alloc(size) { staged = exports.hvc_wasm_alloc(size) >>> 0; return staged; },
            hvc_wasm_free(at) { freed = at >>> 0; return exports.hvc_wasm_free(at); },
            hvc_wasm_response(...args) {
                if (trap) throw new WebAssembly.RuntimeError("injected ingestion trap");
                return exports.hvc_wasm_response(...args);
            },
            hvc_wasm_error() { return tailError ?? exports.hvc_wasm_error(); },
            hvc_wasm_view(...args) {
                const at = exports.hvc_wasm_view(...args);
                if (resolutions !== null && at)
                    new DataView(exports.memory.buffer).setUint32((at >>> 0) + 12, resolutions, true);
                return at;
            },
            hvc_wasm_decode(...args) {
                if (tailError !== null) return -1;
                const result = exports.hvc_wasm_decode(...args);
                if (result < 0) decodeFailures++;
                return result;
            },
        } };
    };
    let channel;
    try {
        channel = await JpipChannel.open(module, server, image);
        assert.equal(initialized, 1, "reactor initialization was not called exactly once");
        resolutions = 33;
        assert.equal(channel.cached(0).quality.length, 33);
        resolutions = 34;
        assert.throws(() => channel.cached(0), /invalid WebAssembly resolution count/);
        resolutions = 0xffffffff;
        assert.throws(() => channel.cached(0), /invalid WebAssembly resolution count/);
        resolutions = null;
        await channel.fetch(0);
        const tail = new Uint8Array(exports.memory.buffer, exports.memory.buffer.byteLength - 17, 17);
        const saved = tail.slice();
        tail.set(new TextEncoder().encode("client is closed\0"));
        tailError = tail.byteOffset;
        try { await assert.rejects(channel.frame(0), { name: "Error", message: "client is closed" }); }
        finally { tail.set(saved); tailError = null; }
        // A different source still needs a response allocation for trap testing.
        await channel.close();
        channel = await JpipChannel.open(module, server, image);
        trap = true;
        freed = null;
        await assert.rejects(channel.frame(0), { name: "RuntimeError", message: "injected ingestion trap" });
        assert.equal(freed, staged, "trap leaked its response allocation");
        await channel.close();
        assert.equal(exports.hvc_wasm_frames(), 0);
        assert.equal(exports.hvc_wasm_codestreams(), 0);
        assert.equal(exports.hvc_wasm_model(0, 2048), 0);
        assert.equal(exports.hvc_wasm_restore_response(0, 0), -1);
        exports.hvc_wasm_cancel_request();
        exports.hvc_wasm_new_channel();
        assert.equal(exports.hvc_wasm_progress(), 0);
        assert.equal(exports.hvc_wasm_decode(0, 0), -1);
        assert.equal(exports.hvc_wasm_xml_size(0), -1);
        assert.equal(exports.hvc_wasm_palette(0), -1);
        assert.equal(exports.hvc_wasm_view(0, 0, 0, 0, 0, 0), 0);
        trap = false;
        unsignedFrames = true;
        channel = await JpipChannel.open(module, server, image);
        assert.equal(channel.frames, 0xffffffff, "an unsigned ABI count became negative in JavaScript");
        await channel.close();
        unsignedFrames = false;
        channel = await JpipChannel.open(module, server, "palette-rgb.jpx");
        const error = /palette indices require one unsigned component of at most 8 bits/;
        await assert.rejects(channel.frame(0), error);
        assert.equal(decodeFailures, 1, "fixture failed before calling the real WASM decoder");
        assert.equal(channel.cached(0).ready, true, "decoder failure discarded the cache");
        await channel.close();

        // Execute the demo's actual open handler. The source, decoder, worker
        // and server are real; only the controls used before failure are stubs.
        const demo = await readFile(`${repository}/client/demo/index.html`, "utf8");
        const handler = demo.slice(demo.indexOf("async function open()"), demo.indexOf("\nconst query ="));
        const source = await JpipSource.open({ wasm: wasmURL, server, image: "palette-rgb.jpx" });
        let closes = 0;
        const openSource = { open: async () => source };
        const close = source.close.bind(source);
        source.close = async () => { closes++; return close(); };
        const execute = new Function("JpipSource", "location", "server", `
            let source = null;
            const pause = () => {};
            const controls = {}, sizes = { replaceChildren() {} }, qualityControls = {}, frames = {};
            const form = { server: { value: server }, image: { value: "palette-rgb.jpx" } };
            const status = {};
            ${handler}
            return open().then(() => ({ source, status, controls }));
        `);
        try {
            const result = await execute(openSource, { href: "http://example.invalid/" }, server);
            assert.match(result.status.textContent, error);
            assert.equal(result.source, null);
            assert.equal(result.controls.disabled, false);
            assert.equal(closes, 1, "demo failed to close its source after decoding failed");
            await assert.rejects(source.frame(0), /source is closed/);
        } finally { await close(); }
    } finally {
        WebAssembly.instantiate = instantiate;
        await channel?.close();
    }
    console.log("WASM imports/exports, initialization, closed-client guards, decode failure, trap and demo cleanup passed");
}
