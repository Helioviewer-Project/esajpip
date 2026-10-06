// Failure cleanup at the JavaScript host boundary, using real workers and HTTP.
import assert from "node:assert/strict";
import { JpipChannel } from "../../client/js/jpip_channel.mjs";
import { JpipSource } from "../../client/js/jpip_source.mjs";
import { Worker } from "./worker.mjs";

export async function checkHost(wasm, wasmURL, server, image) {
    const OriginalMap = globalThis.Map, OriginalWorker = globalThis.Worker;
    const maps = [];
    let worker, id;
    globalThis.Map = class extends OriginalMap {
        constructor(...args) { super(...args); maps.push(this); }
    };
    globalThis.Worker = class extends Worker {
        constructor(...args) { super(...args); worker = this; }
        postMessage(data) { id = data.id; return super.postMessage(data); }
    };
    let opening;
    try { opening = JpipSource.open({ wasm: wasmURL, server, image }); }
    finally { globalThis.Map = OriginalMap; }
    const source = await opening;
    try {
        assert.equal(maps.length, 1, "did not capture the source's pending-call map");
        assert.equal(maps[0].size, 0);
        assert.doesNotThrow(() => worker.onmessage({ data: { id, result: {} } }), "duplicate worker reply threw");
        assert.doesNotThrow(() => worker.onmessage({ data: { id: -1, result: {} } }), "unknown worker reply threw");
        await assert.rejects(source.frame(0, { reduce: () => 0 }), { name: "DataCloneError" });
        assert.equal(maps[0].size, 0, "postMessage failure leaked a pending call");
        assert.ok((await source.frame(0)).pixels.length > 0, "clone failure poisoned subsequent calls");
        await source.close();
        assert.doesNotThrow(() => worker.onmessage({ data: { id, result: {} } }), "late worker reply threw");
    } finally {
        await source.close();
        globalThis.Worker = OriginalWorker;
    }

    const channel = await JpipChannel.open(wasm, server, image);
    const fetch = globalThis.fetch, Bytes = globalThis.Uint8Array;
    let failAllocation = false, cancellations = 0;
    const streams = [];
    globalThis.Uint8Array = new Proxy(Bytes, {
        construct(target, args) {
            if (failAllocation && typeof args[0] === "number" && args[0] > 0) {
                failAllocation = false;
                throw new RangeError("injected receive allocation failure");
            }
            return Reflect.construct(target, args);
        },
    });
    globalThis.fetch = async (url, options) => {
        const response = await fetch(url, options);
        if (new URL(url).searchParams.has("cclose")) return response;
        streams.push(response.body);
        const reader = response.body.getReader();
        return {
            ok: response.ok, status: response.status, headers: response.headers,
            body: { getReader: () => ({
                async read() {
                    const chunk = await reader.read();
                    if (!chunk.done && chunk.value.length) failAllocation = true;
                    return chunk;
                },
                cancel() { cancellations++; return reader.cancel(); },
                releaseLock() { reader.releaseLock(); },
            }) },
        };
    };
    try {
        try {
            await assert.rejects(channel.frame(0), error =>
                error.message === "JPIP transport interrupted" &&
                error.cause?.message === "injected receive allocation failure");
            assert.ok(cancellations > 0, "allocation failure left the response reader uncanceled");
            assert.ok(streams.every(body => !body.locked), "failed response reader stayed locked");
        } finally {
            globalThis.fetch = fetch;
            globalThis.Uint8Array = Bytes;
        }
        assert.ok((await channel.frame(0)).pixels.length > 0);
    } finally { await channel.close(); }
    console.log("Worker duplicate/late replies, clone failure bookkeeping and response-reader cleanup passed");
}
