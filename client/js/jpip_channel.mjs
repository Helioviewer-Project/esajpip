// jpip_channel.mjs: one JPIP channel of an esajpip server: an image or a
// movie, a frame at a time, as 8-bit pixels, and each frame's XML and color
// table. A frame can be fetched ahead of being shown. This file does the
// HTTP exchange; the WebAssembly client (../hvc_wasm.c) keeps the data-bins,
// writes the codestream back and decodes it. It runs in a browser, in a Web
// Worker (jpip_worker.mjs, for jpip_source.mjs) and in Node.js.

// The module is built against a WASI libc, which wants a few system calls
// at start-up and for its standard streams. It has no files and no
// environment, and what it prints is dropped.
function system(module, memory) {
    const view = () => new DataView(memory().buffer);
    const calls = {
        environ_sizes_get(count, size) {
            view().setUint32(count, 0, true);
            view().setUint32(size, 0, true);
            return 0;
        },
        environ_get: () => 0,
        fd_write(fd, vectors, count, written) {
            let total = 0;
            for (let i = 0; i < count; i++)
                total += view().getUint32(vectors + 8 * i + 4, true);
            view().setUint32(written, total, true);
            return 0;
        },
        fd_close: () => 0,
        fd_fdstat_get: () => 52, // WASI ENOTSUP: standard streams are not terminals.
        fd_seek: () => 52,       // Standard streams cannot seek.
        proc_exit(status) {
            throw new Error(`the WebAssembly client exited with status ${status}`);
        },
    };
    const imports = {};
    for (const { module: from, name, kind } of WebAssembly.Module.imports(module)) {
        if (from !== "wasi_snapshot_preview1" || kind !== "function" || !Object.hasOwn(calls, name))
            throw new Error(`unsupported WebAssembly import: ${from}.${name} (${kind})`);
        (imports[from] ??= {})[name] = calls[name];
    }
    return imports;
}

// A JPIP request is a set of fields (T.808 Annex C), written as the query
// of a URL: { stream: 3, fsiz: [512, 512, "closest"] } is
// "stream=3&fsiz=512,512,closest". The server reads them as they are,
// without percent-decoding the JPIP grammar fields.
function query(fields) {
    return Object.entries(fields)
        .map(([name, value]) => `${name}=${Array.isArray(value) ? value.join(",") : value}`)
        .join("&");
}

// The fields of a response header such as JPIP-cnew:
// "cid=5f1c,path=jpip,transport=http" is { cid: "5f1c", path: "jpip", ... }.
function header(value) {
    return Object.fromEntries((value ?? "").split(",").filter(Boolean).map(field => {
        const at = field.indexOf("=");
        return [field.slice(0, at).trim(), field.slice(at + 1).trim()];
    }));
}

function checkOptions(options) {
    if (options === null || typeof options !== "object" || Array.isArray(options) ||
        Object.keys(options).some(key => key !== "fit" && key !== "reduce" && key !== "layers") ||
        ("fit" in options && "reduce" in options))
        throw new RangeError("use { fit: [width, height] } or { reduce: n }");
    if ("fit" in options) {
        if (!Array.isArray(options.fit) || options.fit.length !== 2 ||
            Array.from(options.fit).some(size => !Number.isFinite(size) || size <= 0))
            throw new RangeError("fit must contain two positive finite pixel sizes");
    } else {
        const reduce = "reduce" in options ? options.reduce : 0;
        if (reduce !== Infinity && (!Number.isInteger(reduce) || reduce < 0))
            throw new RangeError("reduce must be a nonnegative integer or Infinity");
    }
    if ("layers" in options && (!Number.isSafeInteger(options.layers) || options.layers < 1))
        throw new RangeError("layers must be a positive safe integer");
}

export class JpipChannel {
    #wasm;                  // the module's exports
    #server;
    #image;                 // original target, also used when replacing a channel
    #target;                // the current channel's routing path
    #cid = null;            // the channel, once the server has assigned it
    #last = Promise.resolve();
    #failed = null;         // why the channel can make no more requests
    #closing = false;
    #controller = null;     // the HTTP exchange under way, including its body
    #timeout;
    #needsRecovery = false; // transport failure; the retained cache is still valid
    frames = 0;             // JPX layers, or one implicit layer for JP2
    received = 0;           // bytes of response bodies so far

    // Opens `image` (a path below the server's image directory) on a new
    // channel. `wasm` is esajpip_client.wasm: compiled (a
    // WebAssembly.Module), its bytes, or a URL to fetch it from. `server`
    // is the server's address, such as http://localhost:8900.
    static async open(wasm, server, image, { timeout = 60000 } = {}) {
        if (!Number.isInteger(timeout) || timeout <= 0 || timeout > 2147483647)
            throw new RangeError("timeout must be a positive integer of milliseconds, at most 2147483647");
        if (typeof wasm === "string" || wasm instanceof URL) {
            const response = await fetch(wasm, { signal: AbortSignal.timeout(timeout) });
            if (!response.ok) {
                await response.body?.cancel().catch(() => {});
                throw new Error(`${response.status} ${wasm}`);
            }
            wasm = await response.arrayBuffer();
        }
        const module = wasm instanceof WebAssembly.Module ? wasm : await WebAssembly.compile(wasm);
        let instance = null;
        instance = await WebAssembly.instantiate(
            module, system(module, () => instance.exports.memory));
        instance.exports._initialize();

        const channel = new JpipChannel();
        channel.#wasm = instance.exports;
        channel.#timeout = timeout;
        channel.#server = new URL(server, globalThis.location?.href).href.replace(/\/+$/, "");
        channel.#image = image.replace(/^\/+/, "").split("/").map(encodeURIComponent).join("/");
        channel.#target = channel.#image;
        // The metadata comes with the first response, whatever it asks for:
        // this one asks for the lowest resolution of codestream zero.
        // Layer zero can refer to another codestream; #fetch resolves it.
        try {
            await channel.#request({ stream: 0, fsiz: [1, 1, "closest"] });
            channel.frames = channel.#wasm.hvc_wasm_frames() >>> 0;
            if (channel.frames === 0)
                throw new Error(channel.#error());
            return channel;
        } catch (error) {
            await channel.close();
            throw error;
        }
    }

    // A frame fitted into options.fit [width, height] in physical pixels,
    // preserving aspect ratio, or without options.reduce highest resolutions
    // (0 for the whole image, Infinity for the lowest), fetching what
    // the channel lacks of it: { index, reduce, width, height, components,
    // pixels, fullWidth, fullHeight, resolutions, layers, totalLayers,
    // quality, complete, ready }. options.layers limits quality, omitted for all.
    // `pixels` is the caller's: a Uint8Array of `components` (1 gray, 3 RGB) values per
    // pixel, rows from the top. Calls are served one at a time, in order.
    frame(index, options = {}) {
        return this.#turn(async () => this.#decode(await this.#fetch(index, options)));
    }

    // Fetches with the same options as frame(), and does not decode it:
    // a later frame() with those options
    // costs no request. Returns its display status, as cached(index, options).
    // Served in turn
    // with the calls of frame(): to fetch a movie ahead, wait for each
    // frame before asking for the next, so that a frame to show waits for
    // one of them at most.
    fetch(index, options = {}) {
        return this.#turn(() => this.#fetch(index, options));
    }

    // Display status for the options, without fetching or decoding. `ready`
    // says the requested quality is cached; `complete` says full quality is.
    // Null until the frame's header is present.
    cached(index, options = {}) {
        checkOptions(options);
        const status = this.#view(index, options);
        const { request, requestedLayers, ...description } = status;
        return status.resolutions === 0 ? null : description;
    }

    // The XML that describes a frame (for a FITS image, its header), or
    // null if the file has none.
    xml(index) {
        this.#checkIndex(index);
        const size = this.#wasm.hvc_wasm_xml_size(index);
        if (size < 0)
            throw new Error(this.#error());
        const at = this.#wasm.hvc_wasm_xml() >>> 0;
        return at === 0 ? null
            : new TextDecoder().decode(new Uint8Array(this.#wasm.memory.buffer, at, size));
    }

    // The color table of a frame, or null if it has none: { entries,
    // channels, table }, where `table` is the caller's: a Uint8Array of
    // `channels` values (3 for red, green, blue) for each of `entries`
    // sample values. It is not applied to the frame's pixels.
    palette(index) {
        this.#checkIndex(index);
        const entries = this.#wasm.hvc_wasm_palette(index);
        if (entries < 0)
            throw new Error(this.#error());
        if (entries === 0)
            return null;
        const channels = this.#wasm.hvc_wasm_palette_channels();
        return {
            entries, channels,
            table: new Uint8Array(this.#wasm.memory.buffer, this.#wasm.hvc_wasm_palette_table() >>> 0,
                                  entries * channels).slice(),
        };
    }

    // Aborts the request under way, forgets the cache, and attempts to close
    // the server channel. A failed server cleanup does not prevent local release.
    close() {
        if (this.#closing) return this.#last;
        this.#closing = true;
        this.#controller?.abort(new Error("the channel is closed"));
        const closed = this.#last.then(async () => {
            this.#failed = new Error("the channel is closed");
            this.#wasm.hvc_wasm_reset();
            this.frames = 0;
            await this.#release();
        });
        this.#last = closed;
        return closed;
    }

    async #release(wait = true) {
        if (this.#cid === null) return;
        const url = this.#url({ cid: this.#cid, cclose: this.#cid });
        this.#cid = null;
        try {
            if (wait) await this.#download(url);
            else fetch(url, { signal: AbortSignal.timeout(this.#timeout) })
                .then(response => response.body?.cancel()).catch(() => {});
        } catch { }
    }

    // Bound silence, not transfer duration. Each received body chunk restarts
    // the timer. Record an assigned channel before reading its body.
    async #download(url, assignChannel = false) {
        const controller = new AbortController();
        this.#controller = controller;
        let timer;
        const progress = () => {
            clearTimeout(timer);
            timer = setTimeout(() => controller.abort(new Error("JPIP request timed out")), this.#timeout);
        };
        progress();
        try {
            const response = await fetch(url, { signal: controller.signal });
            progress();
            if (assignChannel && response.ok) {
                const channel = header(response.headers.get("JPIP-cnew"));
                if (channel.cid !== undefined) {
                    this.#cid = channel.cid;
                    this.#target = channel.path ?? "jpip";
                }
            }
            let body = new Uint8Array(0), size = 0;
            if (response.body !== null) {
                const reader = response.body.getReader();
                try {
                    for (;;) {
                        const { value, done } = await reader.read();
                        if (done) break;
                        if (value.length !== 0) progress();
                        const needed = size + value.length;
                        if (needed > body.length) {
                            const grown = new Uint8Array(Math.max(needed, body.length * 2));
                            grown.set(body.subarray(0, size));
                            body = grown;
                        }
                        body.set(value, size);
                        size = needed;
                    }
                } finally {
                    await reader.cancel().catch(() => {});
                    reader.releaseLock();
                }
            }
            return { response, body: body.subarray(0, size) };
        } catch (error) {
            throw controller.signal.aborted ? controller.signal.reason : error;
        } finally {
            clearTimeout(timer);
            this.#controller = null;
        }
    }

    // A channel serves one request at a time: each call waits for the one
    // before it, whether that succeeded or not.
    #turn(action) {
        const result = this.#last.then(action);
        this.#last = result.catch(() => {});
        return result;
    }

    #url(fields) {
        return `${this.#server}/${this.#target}?${query(fields)}`;
    }

    // Retry an interrupted operation once, after restoring a replacement
    // channel. Transport failure preserves cached frames; ingestion refusal retires the cache.
    async #request(fields) {
        if (this.#failed !== null)
            throw this.#failed;
        let restoring = this.#needsRecovery;
        try {
            if (restoring) {
                await this.#recover();
                this.#needsRecovery = false;
                restoring = false;
                return await this.#exchange(fields);
            }
            try {
                return await this.#exchange(fields);
            } catch (error) {
                if (!error.recoverable || error.timedOut || this.frames === 0 || this.#closing)
                    throw error;
                restoring = true;
                await this.#recover();
                restoring = false;
                return await this.#exchange(fields);
            }
        } catch (error) {
            if ((error.recoverable || restoring) && this.frames !== 0 && !this.#closing) {
                this.#needsRecovery = true;
                this.#wasm.hvc_wasm_cancel_request();
            } else this.#failed = error;
            throw error;
        }
    }

    async #exchange(fields, restoring = false) {
        if (this.#closing) throw new Error("the channel is closed");
        let response, body;
        try {
            ({ response, body } = await this.#download(this.#url(this.#cid === null
                ? { cnew: "http", type: "jpp-stream", ...fields }
                : { cid: this.#cid, ...fields }), true));
        } catch (cause) {
            const error = this.#closing ? new Error("the channel is closed")
                : new Error(cause.message === "JPIP request timed out"
                    ? cause.message : "JPIP transport interrupted", { cause });
            error.recoverable = !this.#closing;
            error.timedOut = cause.message === "JPIP request timed out";
            throw error;
        }
        if (!response.ok) {
            const message = new TextDecoder().decode(body).trim();
            const error = new Error(`${response.status} ${message}`);
            error.recoverable = response.status === 503 &&
                (message === "JPIP channel does not exist" || message === "JPIP channel has ended");
            throw error;
        }
        if (this.#cid === null)
            throw new Error("the server did not assign a channel (JPIP-cnew)");
        this.received += body.length;

        const wasm = this.#wasm;
        const at = wasm.hvc_wasm_alloc(body.length) >>> 0;
        if (at === 0)
            throw new Error("out of memory");
        let reason;
        try {
            new Uint8Array(wasm.memory.buffer, at, body.length).set(body);
            reason = restoring ? wasm.hvc_wasm_restore_response(at, body.length)
                               : wasm.hvc_wasm_response(at, body.length);
        } finally {
            wasm.hvc_wasm_free(at);
        }
        if (reason < 0) {
            const error = new Error(this.#error());
            // Accepted messages may precede a conflicting one. Retire that cache.
            wasm.hvc_wasm_reset();
            this.frames = 0;
            throw error;
        }
        return reason;
    }

    async #recover() {
        // The old channel may still exist if only its response was lost.
        // Closing it is best effort; restoration itself must succeed in full.
        await this.#release(false);
        if (this.#closing)
            throw new Error("the channel is closed");
        this.#target = this.#image;
        let cursor = 0;
        for (;;) {
            if (this.#closing)
                throw new Error("the channel is closed");
            // Selecting the first nonexistent stream avoids receiving headers
            // or precincts before all retained prefixes have been declared.
            const fields = { stream: this.#wasm.hvc_wasm_codestreams() >>> 0, layers: 0 };
            const routing = this.#cid === null ? { cnew: "http", type: "jpp-stream" }
                                               : { cid: this.#cid };
            const url = new URL(this.#url({ ...routing, ...fields, model: "" }));
            const line = `GET ${url.pathname}${url.search} HTTP/1.1\r\n`;
            const capacity = 2048 - new TextEncoder().encode(line).length;
            if (capacity <= 0)
                throw new Error("target path leaves no room for cache declarations");
            const at = this.#wasm.hvc_wasm_model(cursor, capacity) >>> 0;
            if (at === 0)
                throw new Error(this.#error());
            const bytes = new Uint8Array(this.#wasm.memory.buffer, at, Math.min(capacity, 2048));
            const model = new TextDecoder().decode(bytes.subarray(0, bytes.indexOf(0)));
            if (model === "" && this.#cid !== null) break;
            if (model !== "") fields.model = model;
            // Only metadata can be replayed in this window. Its size cannot
            // exceed all bytes received before restoration; allow at most one
            // continuation per such byte, and reject an empty limited reply.
            let remaining = this.received, before = this.received;
            let reason = await this.#exchange(fields, true);
            delete fields.model; // Partial-bin declarations are additive.
            while (reason === 4) {
                if (this.received - before <= 3 || --remaining < 0)
                    throw new Error("replacement channel did not make progress during cache restoration");
                before = this.received;
                reason = await this.#exchange(fields, true);
            }
            if (reason !== 1 && reason !== 2)
                throw new Error("replacement channel did not complete cache restoration");
            cursor = this.#wasm.hvc_wasm_model_next() >>> 0;
        }
    }

    // A frame as the module has it (hvc_frame_status): its size and resolution
    // levels, 0 until its header has arrived, and `complete`, how many of
    // the levels, from the lowest, are cached whole.
    #checkIndex(index) {
        if (this.#closing) throw new Error("the channel is closed");
        if (this.frames === 0 && this.#failed !== null) throw this.#failed;
        if (!Number.isInteger(index) || index < 0 || index >= this.frames)
            throw new RangeError(`no frame ${index}: the image has ${this.frames}`);
    }

    #view(index, options, prepare = false) {
        this.#checkIndex(index);
        const at = this.#wasm.hvc_wasm_view(index,
            Math.min(options.reduce ?? 0, 2147483647),
            options.fit?.[0] ?? 0, options.fit?.[1] ?? 0,
            Math.min(options.layers ?? 0, 2147483647), prepare ? 1 : 0) >>> 0;
        if (at === 0)
            throw new Error(this.#error());
        const [fullWidth, fullHeight, components, resolutions, , totalLayers] =
            new Uint32Array(this.#wasm.memory.buffer, at, 6);
        if (resolutions > 33)
            throw new Error("invalid WebAssembly resolution count");
        const quality = Array.from(new Uint32Array(this.#wasm.memory.buffer, at + 24, resolutions));
        const [reduce, width, height, layers, ready, request, requestedLayers] =
            new Uint32Array(this.#wasm.memory.buffer, at + 156, 7);
        return {
            index, reduce, width, height, components: components >= 3 ? 3 : 1,
            fullWidth, fullHeight, resolutions, layers, totalLayers, quality,
            complete: resolutions > 0 && layers === totalLayers,
            ready: Boolean(ready), request, requestedLayers,
            codestream: Number(new DataView(this.#wasm.memory.buffer).getBigUint64(at + 184, true)),
        };
    }

    // The C client chooses each request and confirms its successful response.
    async #fetch(index, options) {
        checkOptions(options);
        if (this.#closing) throw new Error("the channel is closed");
        if (this.#failed !== null) {
            if (this.frames === 0) throw this.#failed;
            const { request, requestedLayers, ...status } = this.#view(index, options);
            if (status.ready) return status;
            throw this.#failed;
        }
        for (;;) {
            const { request, requestedLayers, ...status } = this.#view(index, options, true);
            if (request === 0) return status;
            const fields = request === 1 ? { stream: status.codestream, layers: 0 }
                : { stream: status.codestream, fsiz: [status.width, status.height, "closest"] };
            if (request === 2 && "layers" in options) fields.layers = requestedLayers;
            const reason = await this.#request(fields);
            if (reason !== 1 && reason !== 2)
                throw new Error("the server did not complete the requested quality layers");
        }
    }

    // Decodes a frame from the cache. The pixels are copied out of the
    // module, which keeps its own until the next decode.
    #decode(status) {
        const wasm = this.#wasm;
        if (wasm.hvc_wasm_decode(status.index, status.reduce) !== 0)
            throw new Error(this.#error());
        const width = wasm.hvc_wasm_width(), height = wasm.hvc_wasm_height();
        const components = wasm.hvc_wasm_components();
        return {
            ...status, width, height, components,
            pixels: new Uint8Array(wasm.memory.buffer, wasm.hvc_wasm_pixels() >>> 0,
                                   width * height * components).slice(),
        };
    }

    #error() {
        const at = this.#wasm.hvc_wasm_error() >>> 0;
        const buffer = this.#wasm.memory.buffer;
        const bytes = new Uint8Array(buffer, at, Math.min(256, buffer.byteLength - at));
        return new TextDecoder().decode(bytes.subarray(0, bytes.indexOf(0)));
    }
}
