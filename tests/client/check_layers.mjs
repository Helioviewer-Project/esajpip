// Generate fixtures with test_client_source --write-fixtures <server directory>.
// node check_layers.mjs <wasm> <server URL>
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { JpipChannel } from "../../client/js/jpip_channel.mjs";
const [path, server] = process.argv.slice(2);
const wasm = await readFile(path);
for (const [image, frames] of [["one-layer.jpx", 1], ["swapped.jpx", 2]]) {
    const channel = await JpipChannel.open(wasm, server, image);
    const originalFetch = globalThis.fetch;
    const requests = [];
    let expire = false;
    globalThis.fetch = (url, options) => {
        requests.push(new URL(url));
        if (expire) {
            expire = false;
            return Promise.resolve(new Response("JPIP channel does not exist", { status: 503 }));
        }
        return originalFetch(url, options);
    };
    try {
        assert.equal(channel.frames, frames);
        assert.equal(channel.xml(0), "B");
        let first = await channel.frame(0, { reduce: Infinity });
        assert.equal(first.codestream, 1);
        assert.ok(first.ready && first.pixels.length > 0);
        assert.ok(requests.length > 0);
        assert.ok(requests.every(url => url.searchParams.get("stream") === "1"));
        requests.length = 0;
        expire = true;
        first = await channel.frame(0);
        const declarations = requests.filter(url => url.searchParams.has("model"));
        assert.ok(declarations.length > 0);
        assert.ok(declarations.every(url => url.searchParams.get("stream") === "2"));
        assert.equal(first.codestream, 1);
        if (frames === 2) {
            requests.length = 0;
            assert.equal(channel.xml(1), "A");
            const second = await channel.frame(1);
            assert.equal(second.codestream, 0);
            assert.deepEqual(second.pixels, first.pixels);
            assert.ok(requests.every(url => url.searchParams.get("stream") === "0"));
        }
    } finally {
        globalThis.fetch = originalFetch;
        await channel.close();
    }
    console.log(`${image}: layer count, mapped requests, XML and pixels passed`);
}
