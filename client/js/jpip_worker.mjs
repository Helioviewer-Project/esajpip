// jpip_worker.mjs: a JPIP channel (jpip_channel.mjs) in a Web Worker, for
// jpip_source.mjs, so that fetching and decoding do not hold up the page.
// The page posts { id, call, args } and gets { id, result } or { id, error }
// back.
import { JpipChannel } from "./jpip_channel.mjs";

let channel = null;

// Each gives its result and the buffers to move, not copy, to the page.
const calls = {
    // The arguments of JpipChannel.open.
    async open(wasm, server, image, options) {
        channel = await JpipChannel.open(wasm, server, image, options);
        return [{ frames: channel.frames, received: channel.received }, []];
    },
    // The arguments of JpipChannel.frame.
    async frame(index, options) {
        const frame = await channel.frame(index, options);
        return [{ frame, received: channel.received }, [frame.pixels.buffer]];
    },
    // The arguments of JpipChannel.fetch.
    async fetch(index, options) {
        return [{ status: await channel.fetch(index, options), received: channel.received }, []];
    },
    cached(index, options) {
        return [channel.cached(index, options), []];
    },
    xml(index) {
        return [channel.xml(index), []];
    },
    palette(index) {
        const palette = channel.palette(index);
        return [palette, palette === null ? [] : [palette.table.buffer]];
    },
    async close() {
        await channel.close();
        return [null, []];
    },
};

self.onmessage = async ({ data: { id, call, args } }) => {
    try {
        if (channel === null && call !== "open")
            throw new Error("no image is open");
        const [result, moved] = await calls[call](...args);
        self.postMessage({ id, result }, moved);
    } catch (error) {
        self.postMessage({ id, error: { name: error.name, message: error.message } });
    }
};
