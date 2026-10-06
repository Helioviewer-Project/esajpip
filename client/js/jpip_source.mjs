// jpip_source.mjs: an image or a movie of an esajpip server, for a page: its
// frames as 8-bit pixels and each frame's XML. The work is done by a JPIP
// channel (jpip_channel.mjs) in a Web Worker of its own (jpip_worker.mjs),
// so a page may have several sources open, and none holds it up.
//
//   const source = await JpipSource.open({ wasm, server, image });
//   const frame = await source.frame(3, { fit: [1024, 768] });
//   gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, frame.width, frame.height, 0,
//                 gl.RED, gl.UNSIGNED_BYTE, frame.pixels);
//   const xml = await source.xml(3);
//   const palette = await source.palette(3);    // for a lookup texture
//   await source.fetch(4);                      // ahead of showing it
//   await source.close();

// esajpip_client.wasm, compiled once for all the sources of a page.
const modules = new Map();
function compiled(wasm) {
    const url = String(wasm);
    if (!modules.has(url))
        modules.set(url, fetch(url, { signal: AbortSignal.timeout(60000) }).then(async response => {
            if (!response.ok)
                throw new Error(`${response.status} ${url}`);
            return WebAssembly.compile(await response.arrayBuffer());
        }).catch(error => {
            modules.delete(url);
            throw error;
        }));
    return modules.get(url);
}

export class JpipSource {
    #worker;
    #waiting = new Map();   // call -> what settles its promise
    #calls = 0;
    frames = 0;             // codestreams of the target: the frames of a movie
    received = 0;           // bytes of response bodies so far

    // Opens `image` (a path below the server's image directory) on a JPIP
    // channel of `server` (such as http://localhost:8900). `wasm` is the
    // URL of esajpip_client.wasm.
    static async open({ wasm, server, image, timeout = 60000 }) {
        server = new URL(server, globalThis.location?.href).href;
        const source = new JpipSource();
        const module = await compiled(wasm);
        source.#worker = new Worker(new URL("./jpip_worker.mjs", import.meta.url),
                                    { type: "module" });
        source.#worker.onmessage = ({ data: { id, result, error } }) => {
            const { resolve, reject } = source.#waiting.get(id);
            source.#waiting.delete(id);
            if (error === undefined)
                resolve(result);
            else
                reject(error.name === "RangeError" ? new RangeError(error.message)
                                                   : new Error(error.message));
        };
        source.#worker.onerror = () => source.#end(new Error("the worker failed"));
        try {
            ({ frames: source.frames, received: source.received } =
                await source.#call("open", module, server, image, { timeout }));
        } catch (error) {
            source.#end(error);
            throw error;
        }
        return source;
    }

    // A frame (0 to frames - 1) fitted into options.fit [width, height]
    // in physical pixels, preserving aspect ratio, or with options.reduce
    // (0 for full size, Infinity for the lowest), fetching what is missing:
    // { index, reduce, width, height, components, pixels, fullWidth, fullHeight,
    // resolutions, layers, totalLayers, quality, complete, ready }. options.layers
    // limits quality, omitted for all. `pixels` is the caller's: a Uint8Array
    // of `components`
    // (1 gray, 3 RGB) values per pixel, rows from the top. Calls are served
    // one at a time, in order.
    async frame(index, options = {}) {
        const { frame, received } = await this.#call("frame", index, options);
        this.received = received;
        return frame;
    }

    // Fetches with the same options as frame(), and does not decode it:
    // a later frame() with those options
    // costs no request. Returns its display status, as cached(index, options).
    // Served in turn
    // with the calls of frame(): to fetch a movie ahead, wait for each
    // frame before asking for the next, so that a frame to show waits for
    // one of them at most.
    async fetch(index, options = {}) {
        const { status, received } = await this.#call("fetch", index, options);
        this.received = received;
        return status;
    }

    // Display status for the options, without fetching or decoding. `ready`
    // says the requested quality is cached; `complete` says full quality is.
    // Null until the frame's header is present.
    cached(index, options = {}) {
        return this.#call("cached", index, options);
    }

    // The XML that describes a frame (for a FITS image, its header), or
    // null if the file has none.
    xml(index) {
        return this.#call("xml", index);
    }

    // The color table of a frame, or null if it has none: { entries,
    // channels, table }, where `table` is the caller's: a Uint8Array of
    // `channels` values (3 for red, green, blue) for each of `entries`
    // sample values. It is not applied to the frame's pixels.
    palette(index) {
        return this.#call("palette", index);
    }

    // Aborts the request under way, releases the source and its worker, and
    // attempts to close the server channel.
    async close() {
        if (this.#worker === null) return;
        try {
            await this.#call("close");
        } finally {
            this.#end(new Error("the source is closed"));
        }
    }

    #call(name, ...args) {
        return new Promise((resolve, reject) => {
            if (this.#worker === null)
                throw new Error("the source is closed");
            this.#waiting.set(++this.#calls, { resolve, reject });
            this.#worker.postMessage({ id: this.#calls, call: name, args });
        });
    }

    #end(error) {
        this.#worker?.terminate();
        this.#worker = null;
        for (const { reject } of this.#waiting.values())
            reject(error);
        this.#waiting.clear();
    }
}
