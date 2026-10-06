// Checks the channel API, movie decoding and worker against a local server:
// node tests/client/check.mjs <built esajpip_client.wasm> <server URL> <image>
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { createServer } from "node:http";
import { Worker } from "./worker.mjs";
import { JpipChannel } from "../../client/js/jpip_channel.mjs";
import { JpipSource } from "../../client/js/jpip_source.mjs";

const [wasmPath, server, image] = process.argv.slice(2);
if (image === undefined)
    throw new Error("usage: check.mjs wasm server image");
const wasm = await readFile(wasmPath);

// FNV-1a, 32 bits.
function checksum(bytes) {
    let hash = 0x811c9dc5;
    for (const byte of bytes)
        hash = Math.imul(hash ^ byte, 0x01000193);
    return (hash >>> 0).toString(16).padStart(8, "0");
}

function report(channel, frame) {
    const xml = channel.xml(frame.index);
    const palette = channel.palette(frame.index);
    console.log(`frame ${frame.index}: ${frame.width}x${frame.height}, ` +
                `${frame.components} component(s), ${channel.received} bytes received, ` +
                `pixels ${checksum(frame.pixels)}, ` +
                (xml === null ? "no XML" : `XML ${new TextEncoder().encode(xml).length} bytes`) +
                (palette === null ? "" : `, color table of ${palette.entries}`));
}

// Only the browser's worker transport is adapted; jpip_worker.mjs runs unchanged.
globalThis.Worker = Worker;

const host = createServer((request, response) => response.end(wasm));
await new Promise(resolve => host.listen(0, "127.0.0.1", resolve));
try {
    const channel = await JpipChannel.open(wasm, server, image);
    const requests = [];
    const originalFetch = globalThis.fetch;
    globalThis.fetch = (url, options) => {
        requests.push(new URL(url));
        return originalFetch(url, options);
    };
    try {
        const received = channel.received;
        for (const index of [-1, channel.frames, 0.5, NaN]) {
            for (const method of ["cached", "xml", "palette"])
                assert.throws(() => channel[method](index), RangeError);
            for (const method of ["frame", "fetch"])
                await assert.rejects(channel[method](index), RangeError);
        }
        for (const reduce of [-1, 0.5, NaN, "1"])
            for (const method of ["frame", "fetch"])
                await assert.rejects(channel[method](0, { reduce }), RangeError);
        for (const options of [null, 2, [], { reduce: null }, { reduce: undefined },
                               { fit: [1, 1], reduce: 0 }, { size: [1, 1] },
                               { fit: [0, 1] }, { fit: [-1, 1] }, { fit: [NaN, 1] },
                               { fit: [Infinity, 1] }, { fit: [1] }, { fit: [1, 2, 3] },
                               { fit: [, 1] }, { fit: ["1", 1] },
                               ...[0, -1, 0.5, NaN, Infinity, "1", null, undefined,
                                   Number.MAX_SAFE_INTEGER + 1].map(layers => ({ layers }))]) {
            assert.throws(() => channel.cached(channel.frames - 1, options), RangeError);
            for (const method of ["frame", "fetch"])
                await assert.rejects(channel[method](channel.frames - 1, options), RangeError);
        }
        assert.equal(requests.length, 0, "invalid calls sent a request");
        assert.equal(channel.received, received, "invalid calls sent a request");
        const first = await channel.frame(0, { reduce: Infinity });
        assert.equal(first.reduce, channel.cached(0, { reduce: Infinity }).reduce);
        assert.equal(channel.cached(0).ready, first.reduce === 0);
        assert.equal(channel.received, received, "cached frame sent a request");

        console.log(`${channel.frames} frame(s)`);
        for (let reduce = first.reduce; reduce >= 0; reduce--)
            report(channel, await channel.frame(0, { reduce }));
        const full = await channel.frame(0);
        const beforeFit = requests.length;
        for (let reduce = 0; reduce < full.resolutions; reduce++) {
            const width = Math.ceil(full.fullWidth / 2 ** reduce);
            const height = Math.ceil(full.fullHeight / 2 ** reduce);
            // One limiting dimension; the other has ample unused space.
            // Covering the raw rectangle would wrongly select a finer level.
            for (const fit of [[width, full.fullHeight * 4], [full.fullWidth * 4, height]]) {
                const frame = await channel.frame(0, { fit });
                const scale = Math.min(fit[0] / full.fullWidth, fit[1] / full.fullHeight);
                assert.ok(frame.width >= full.fullWidth * scale &&
                          frame.height >= full.fullHeight * scale, `fit too coarse for ${fit}`);
                if (frame.reduce + 1 < full.resolutions)
                    assert.ok(Math.ceil(full.fullWidth / 2 ** (frame.reduce + 1)) < full.fullWidth * scale ||
                              Math.ceil(full.fullHeight / 2 ** (frame.reduce + 1)) < full.fullHeight * scale,
                              `fit too fine for ${fit}`);
                const explicit = await channel.frame(0, { reduce: frame.reduce });
                assert.deepEqual(frame.pixels, explicit.pixels);
            }
            if (reduce > 0 && width < full.fullWidth) {
                const frame = await channel.frame(0, { fit: [width + 0.25, full.fullHeight * 4] });
                assert.ok(frame.reduce < reduce, "fit did not cross a resolution boundary");
            }
        }
        assert.equal((await channel.frame(0, { fit: [full.fullWidth * 2, full.fullHeight * 2] })).reduce, 0);
        assert.equal((await channel.frame(0, { fit: [0.25, 0.25] })).reduce, full.resolutions - 1);
        assert.equal(requests.length, beforeFit, "fit requested already cached levels");
        for (let index = 1; index < channel.frames; index++) {
            const start = requests.length;
            const options = { fit: [20, 30], layers: 1 };
            assert.equal(channel.cached(index, options), null, "missing header was fetched by cached()");
            const cached = await channel.fetch(index, options);
            assert.equal(cached.ready, true);
            assert.equal("pixels" in cached, false);
            assert.equal(requests.length - start, 2, `frame ${index} needs a header and pixel request`);
            const header = requests[start].searchParams;
            assert.equal(header.get("stream"), String(index));
            assert.equal(header.get("layers"), "0");
            assert.equal(header.has("fsiz"), false);
            const received = channel.received;
            const frame = await channel.frame(index, options);
            const scale = Math.min(20 / frame.fullWidth, 30 / frame.fullHeight);
            if (frame.reduce > 0) {
                assert.ok(frame.width >= frame.fullWidth * scale);
                assert.ok(frame.height >= frame.fullHeight * scale);
            }
            if (frame.reduce + 1 < frame.resolutions)
                assert.ok(Math.ceil(frame.fullWidth / 2 ** (frame.reduce + 1)) < frame.fullWidth * scale ||
                          Math.ceil(frame.fullHeight / 2 ** (frame.reduce + 1)) < frame.fullHeight * scale,
                          "fit did not select the coarsest usable level");
            const pixels = requests[start + 1].searchParams;
            assert.equal(pixels.get("stream"), String(index));
            assert.equal(pixels.get("layers"), "1");
            assert.equal(pixels.get("fsiz"),
                `${Math.ceil(frame.fullWidth / 2 ** frame.reduce)},${Math.ceil(frame.fullHeight / 2 ** frame.reduce)},closest`);
            assert.equal(requests.length - start, 2, `cached frame ${index} made another request`);
            assert.deepEqual(cached, channel.cached(index, options));
            const { pixels: decoded, ...frameStatus } = frame;
            assert.deepEqual(cached, frameStatus, `frame ${index} differs from fetch status`);
            assert.ok(frame.layers >= 1, `frame ${index} preview has no confirmed layers`);
            assert.equal(channel.received, received, `frame ${index} sent a request after fetch`);
            report(channel, frame);
            const refined = await channel.frame(index, { fit: [20, 30] });
            assert.equal(refined.complete, true, `frame ${index} did not refine to full quality`);
        }
    } finally {
        globalThis.fetch = originalFetch;
        await channel.close();
    }

    const source = await JpipSource.open({
        wasm: `http://127.0.0.1:${host.address().port}/client.wasm`, server, image,
    });
    try {
        for (const method of ["cached", "xml", "palette", "frame", "fetch"])
            await assert.rejects(source[method](source.frames), RangeError);
        await assert.rejects(source.frame(0, { reduce: NaN }), RangeError);
        await assert.rejects(source.fetch(0, { fit: [0, 1] }), RangeError);
        const first = await source.frame(0, { reduce: Infinity });
        assert.equal(first.reduce, (await source.cached(0, { reduce: Infinity })).reduce);
        if (source.frames > 1) assert.equal(await source.cached(1), null);
        await assert.rejects(source.cached(0, { layers: 0 }), RangeError);
        assert.equal(first.pixels.length, first.width * first.height * first.components);
        const received = source.received;
        const repeated = await source.frame(0, { fit: [first.width, first.height] });
        assert.equal(repeated.reduce, first.reduce);
        assert.deepEqual(repeated.pixels, first.pixels);
        assert.equal(source.received, received);
        const cached = await source.fetch(0, { fit: [first.fullWidth, first.fullHeight] });
        assert.equal(cached.reduce, 0);
        assert.equal(cached.ready, true);
        assert.equal(cached.complete, true);
        const afterFetch = source.received;
        const full = await source.frame(0, { fit: [first.fullWidth, first.fullHeight] });
        assert.equal(full.reduce, 0);
        assert.equal(source.received, afterFetch);
        await source.xml(0);
        await source.palette(0);
    } finally {
        await source.close();
    }
    await assert.rejects(source.frame(0), /source is closed/);

    const progressive = await JpipSource.open({
        wasm: `http://127.0.0.1:${host.address().port}/client.wasm`, server, image,
    });
    const reference = await JpipChannel.open(wasm, server, image);
    try {
        const start = progressive.received;
        const preview = await progressive.frame(0, { fit: [512, 512], layers: 1 });
        const previewBytes = progressive.received - start;
        assert.ok(preview.layers >= 1 && preview.totalLayers >= preview.layers);
        assert.equal(preview.ready, true);
        assert.equal(preview.complete, preview.layers === preview.totalLayers);
        const previewStatus = await progressive.cached(0, { fit: [512, 512], layers: 1 });
        const { pixels: previewPixels, ...shownStatus } = preview;
        assert.deepEqual(previewStatus, shownStatus);
        const fullStatus = await progressive.cached(0, { fit: [512, 512] });
        assert.equal(fullStatus.ready, preview.complete);
        const repeated = await progressive.frame(0, { fit: [512, 512], layers: 1 });
        assert.deepEqual(repeated.pixels, preview.pixels);
        assert.equal(progressive.received - start, previewBytes, "preview repeat requested data");
        const before = reference.received;
        await reference.frame(0, { fit: [512, 512] });
        const fullBytes = reference.received - before;
        assert.ok(previewBytes <= fullBytes, "preview used more data than full quality");

        const prefetched = await progressive.fetch(0, { fit: [1024, 1024], layers: 1 });
        assert.equal(prefetched.ready, true);
        assert.ok(prefetched.layers >= 1);
        const widerBytes = progressive.received;
        const wider = await progressive.frame(0, { fit: [1024, 1024], layers: 1 });
        assert.equal(progressive.received, widerBytes, "preview fetch did not populate cache");
        assert.ok(wider.reduce <= preview.reduce && wider.layers >= 1);
        const { pixels: widerPixels, ...widerStatus } = wider;
        assert.deepEqual(prefetched, widerStatus);
        const refined = await progressive.frame(0, { fit: [512, 512], layers: 2 });
        assert.ok(refined.layers >= Math.min(2, refined.totalLayers));
        const final = await progressive.frame(0, { fit: [1024, 1024] });
        const expected = await reference.frame(0, { fit: [1024, 1024] });
        assert.equal(final.complete, true);
        assert.equal(final.layers, final.totalLayers);
        assert.deepEqual(final.pixels, expected.pixels, "refinement differs from full transfer");
        assert.equal((await progressive.cached(0, { fit: [1024, 1024] })).reduce, final.reduce);
        const finalBytes = progressive.received;
        await progressive.frame(0, { fit: [1024, 1024], layers: Number.MAX_SAFE_INTEGER });
        assert.equal(progressive.received, finalBytes, "clamped layers requested cached data");
        console.log(`512px preview: ${previewBytes} bytes; full quality: ${fullBytes} bytes`);
    } finally {
        await progressive.close();
        await reference.close();
    }

    const interrupted = await JpipChannel.open(wasm, server, image);
    let interruptedRequests = 0;
    globalThis.fetch = async (url, options) => {
        const response = await originalFetch(url, options);
        if (++interruptedRequests !== 1) return response;
        const bytes = new Uint8Array(await response.arrayBuffer());
        assert.deepEqual(Array.from(bytes.slice(-3)), [0, 2, 0]);
        bytes[bytes.length - 2] = 4; // Byte limit, not completion of the window.
        return new Response(bytes, { status: response.status, headers: response.headers });
    };
    try {
        await assert.rejects(interrupted.frame(0, { fit: [512, 512], layers: 1 }),
                             /did not complete the requested quality layers/);
        await interrupted.frame(0, { fit: [512, 512], layers: 1 });
        assert.equal(interruptedRequests, 2, "incomplete response was recorded as delivered quality");
    } finally {
        globalThis.fetch = originalFetch;
        await interrupted.close();
    }
    console.log("Channel, decoding and worker checks passed");
} finally {
    host.closeAllConnections();
    await new Promise(resolve => host.close(resolve));
}
