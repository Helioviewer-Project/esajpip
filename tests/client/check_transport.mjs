// Real HTTP regressions: stalled headers/bodies, failed opens and cleanup.
import assert from "node:assert/strict";
import { setTimeout as delay } from "node:timers/promises";
import { createServer } from "node:http";
import { JpipChannel } from "../../client/js/jpip_channel.mjs";
import { JpipSource } from "../../client/js/jpip_source.mjs";
import { Worker } from "./worker.mjs";

export async function checkTransport(wasm, server, image) {
    let mode = "pass", entered, stalled, repeatedReply;
    const requests = [], assigned = [], closed = [], limitedReasons = [];
    const cleanup = new Map();
    const pending = new Set();
    const proxy = createServer(async (request, response) => {
        const url = new URL(request.url, server);
        requests.push(url);
        if (url.pathname === "/client.wasm") { response.end(wasm); return; }
        const cid = url.searchParams.get("cclose");
        if (cid) {
            closed.push(cid);
            if (mode === "cleanup-fails") { response.destroy(); return; }
            if (mode === "cleanup-stalls") { pending.add(response); return; }
        }
        try {
            if (mode === "recovery-unavailable" && url.searchParams.has("model")) {
                response.writeHead(503);
                response.end("temporary service outage");
                return;
            }
            if (mode === "headers" && !cid && !url.searchParams.has("model")) {
                pending.add(response);
                entered?.();
                return;
            }
            const upstreamURL = new URL(request.url, server);
            const restoring = !cid && url.searchParams.get("layers") === "0" &&
                !url.searchParams.has("fsiz");
            if (restoring && mode === "repeating-limit" && repeatedReply) {
                response.writeHead(repeatedReply.status, repeatedReply.headers);
                response.end(repeatedReply.body);
                return;
            }
            if (restoring && ["byte-limit", "empty-limit", "repeating-limit"].includes(mode)) {
                // Exercise real server limits. Make it replay metadata even
                // when the host advertises M0, as another server may do.
                const model = upstreamURL.searchParams.get("model");
                if (model !== null) {
                    const bins = model.split(",").filter(bin => !/^M0(?:$|:)/.test(bin));
                    if (bins.length) upstreamURL.searchParams.set("model", bins.join(","));
                    else upstreamURL.searchParams.delete("model");
                }
                // The writer reserves 60 bytes, so 96 permits small contributions.
                upstreamURL.searchParams.set("len", mode === "empty-limit" ? "3" : "96");
            }
            const upstream = await fetch(upstreamURL);
            const cnew = /(?:^|,)cid=([^,]+)/.exec(upstream.headers.get("JPIP-cnew") ?? "");
            if (cnew) assigned.push(cnew[1]);
            const body = new Uint8Array(await upstream.arrayBuffer());
            if (restoring && mode === "byte-limit") limitedReasons.push(body.at(-2));
            if (restoring && mode === "repeating-limit") {
                assert.equal(body.at(-2), 4);
                repeatedReply = { status: upstream.status, headers: Object.fromEntries(upstream.headers), body };
            }
            response.writeHead(upstream.status, Object.fromEntries(upstream.headers));
            if ((mode === "body" && !cid && !url.searchParams.has("model")) ||
                (mode === "open-body" && cnew)) {
                response.write(body.subarray(0, 1));
                pending.add(response);
                entered?.();
            } else if (mode === "progress" && !cid && !url.searchParams.has("model")) {
                const step = Math.ceil(body.length / 8);
                for (let offset = 0; offset < body.length; offset += step) {
                    response.write(body.subarray(offset, offset + step));
                    await delay(75);
                }
                response.end();
            } else if (mode === "recovery-stalls" && url.searchParams.has("model")) {
                response.write(body.subarray(0, 1));
                pending.add(response);
            } else if (mode === "all-stalls") {
                pending.add(response);
            } else if (mode === "refuse-open" && cnew) {
                response.end(new Uint8Array([0]));
            } else {
                response.end(body);
                if (cid) cleanup.set(cid, true);
            }
        } catch (error) {
            response.destroy(error);
        }
    });
    await new Promise(resolve => proxy.listen(0, "127.0.0.1", resolve));
    const address = `http://127.0.0.1:${proxy.address().port}`;
    const open = () => JpipChannel.open(wasm, address, image, { timeout: 250 });
    const sources = [];
    try {
        for (const setting of ["headers", "body"]) {
            mode = "pass";
            const channel = await open(); sources.push(channel);
            requests.length = 0;
            mode = setting;
            // Each silent request costs one idle deadline, with no immediate
            // timeout retry. A later call can recover without losing the cache.
            const started = performance.now();
            await assert.rejects(channel.frame(0), /timed out/);
            assert.ok(performance.now() - started < 500, "silent data request used multiple deadlines");
            assert.equal(requests.filter(url => url.searchParams.has("cnew")).length, 0);
            mode = "pass";
            assert.ok((await channel.frame(0)).pixels.length > 0, "timeout poisoned future recovery");
            await channel.close();
        }
        mode = "pass";
        const progressing = await open(); sources.push(progressing);
        mode = "progress";
        const started = performance.now();
        assert.ok((await progressing.frame(0)).pixels.length > 0);
        assert.ok(performance.now() - started > 500, "progress test did not exceed the idle timeout");
        await progressing.close();

        mode = "pass";
        const recovering = await open(); sources.push(recovering);
        await fetch(`${server}/jpip?cid=${assigned.at(-1)}&cclose=${assigned.at(-1)}`);
        mode = "recovery-stalls";
        await assert.rejects(recovering.frame(0), /timed out/);
        mode = "pass";
        assert.ok((await recovering.frame(0)).pixels.length > 0, "failed recovery poisoned the retained cache");
        await recovering.close();

        mode = "pass";
        const unavailable = await open(); sources.push(unavailable);
        const retained = unavailable.cached(0, { reduce: Infinity });
        await fetch(`${server}/jpip?cid=${assigned.at(-1)}&cclose=${assigned.at(-1)}`);
        mode = "recovery-unavailable";
        await assert.rejects(unavailable.frame(0), /503/);
        assert.deepEqual(unavailable.cached(0, { reduce: Infinity }), retained);
        mode = "pass";
        assert.ok((await unavailable.frame(0)).pixels.length > 0, "HTTP outage during recovery poisoned the cache");
        await unavailable.close();

        mode = "pass";
        const silent = await open(); sources.push(silent);
        mode = "all-stalls";
        const silence = performance.now();
        await assert.rejects(silent.frame(0), /timed out/);
        assert.ok(performance.now() - silence < 500, "silent server cost three deadlines");
        mode = "pass";
        await silent.close();

        mode = "pass";
        const closing = await open(); sources.push(closing);
        mode = "body";
        stalled = new Promise(resolve => { entered = resolve; });
        const rejected = assert.rejects(closing.frame(0), /channel is closed/);
        const queued = assert.rejects(closing.frame(0), /channel is closed/);
        await stalled;
        mode = "cleanup-fails";
        await closing.close();
        await Promise.all([rejected, queued]);
        await closing.close();
        assert.equal(closing.frames, 0);
        assert.throws(() => closing.cached(0), /channel is closed/);
        entered = null;

        for (const setting of ["open-body", "refuse-open"]) {
            mode = setting;
            const count = assigned.length;
            const started = performance.now();
            await assert.rejects(open());
            if (setting === "open-body")
                assert.ok(performance.now() - started < 500, "failed open waited for cleanup after timing out");
            assert.equal(assigned.length, count + 1);
            for (let attempt = 0; attempt < 50 && !cleanup.has(assigned.at(-1)); attempt++) await delay(10);
            assert.ok(cleanup.has(assigned.at(-1)), "failed open leaked its assigned channel");
            const reply = await fetch(`${server}/jpip?cid=${assigned.at(-1)}&stream=0`);
            assert.equal(reply.status, 503, "failed open left the server channel alive");
        }

        mode = "pass";
        const limited = await open(); sources.push(limited);
        const preview = await limited.frame(0, { reduce: Infinity });
        const limitedRetained = limited.cached(0, { reduce: Infinity });
        const metadata = limited.xml(0);
        const cid = assigned.at(-1);
        await fetch(`${server}/jpip?cid=${cid}&cclose=${cid}`);
        mode = "byte-limit";
        requests.length = 0;
        limitedReasons.length = 0;
        assert.ok((await limited.frame(0)).pixels.length > 0);
        const restoration = requests.filter(url => url.searchParams.get("layers") === "0" &&
            !url.searchParams.has("fsiz"));
        assert.ok(restoration.length > 2, "server limit did not require continuation");
        assert.ok(limitedReasons.includes(4), "server did not produce a real byte-limit EOR");
        assert.equal(limitedReasons.at(-1), 2);
        assert.equal(restoration.filter(url => url.searchParams.has("model")).length, 1,
                     "continuations repeated additive model declarations");
        assert.ok(restoration.every(url => url.searchParams.get("stream") === "1"));
        assert.equal(new Set(restoration.slice(1).map(url => url.searchParams.get("cid"))).size, 1);
        assert.equal(limited.xml(0), metadata);
        assert.deepEqual((await limited.frame(0, { reduce: Infinity })).pixels, preview.pixels);
        assert.ok(limited.cached(0, { reduce: Infinity }).ready && limitedRetained.ready);
        mode = "pass";
        await limited.close();

        const empty = await open(); sources.push(empty);
        await fetch(`${server}/jpip?cid=${assigned.at(-1)}&cclose=${assigned.at(-1)}`);
        mode = "empty-limit";
        requests.length = 0;
        await assert.rejects(empty.frame(0), /did not make progress during cache restoration/);
        assert.equal(requests.filter(url => url.searchParams.get("layers") === "0" &&
            !url.searchParams.has("fsiz")).length, 1, "empty byte-limit reply looped");
        mode = "pass";
        assert.ok((await empty.frame(0)).pixels.length > 0, "empty restoration reply poisoned the cache");
        await empty.close();

        const repeating = await open(); sources.push(repeating);
        await fetch(`${server}/jpip?cid=${assigned.at(-1)}&cclose=${assigned.at(-1)}`);
        mode = "repeating-limit";
        repeatedReply = null;
        requests.length = 0;
        // Replay a real capped reply unchanged to simulate a server that
        // responds forever without advancing its metadata offset.
        await assert.rejects(repeating.frame(0), /did not make progress during cache restoration/);
        const repeats = requests.filter(url => url.searchParams.get("layers") === "0" &&
            !url.searchParams.has("fsiz"));
        assert.equal(repeats.length, 2, "repeated metadata was not stopped on its first replay");
        mode = "pass";
        assert.ok((await repeating.frame(0)).pixels.length > 0);
        await repeating.close();

        const boundedCleanup = await open(); sources.push(boundedCleanup);
        mode = "cleanup-stalls";
        await boundedCleanup.close();
        assert.equal(boundedCleanup.frames, 0);

        // Resolve on the page before passing a relative server to its worker.
        mode = "pass";
        globalThis.Worker = Worker;
        globalThis.location = { href: `${address}/page/index.html` };
        const beforeFailedOpen = assigned.length;
        mode = "open-body";
        await assert.rejects(JpipSource.open({ wasm: `${address}/client.wasm`, server: "../..", image, timeout: 250 }),
                             /timed out/);
        assert.equal(assigned.length, beforeFailedOpen + 1);
        for (let attempt = 0; attempt < 50 && !cleanup.has(assigned.at(-1)); attempt++) await delay(10);
        assert.ok(cleanup.has(assigned.at(-1)), "failed worker open leaked its channel");
        mode = "pass";
        const source = await JpipSource.open({ wasm: `${address}/client.wasm`, server: "../..", image, timeout: 250 });
        try {
            const before = assigned.length;
            await fetch(`${server}/jpip?cid=${assigned.at(-1)}&cclose=${assigned.at(-1)}`);
            await source.frame(0);
            assert.equal(assigned.length, before + 1, "relative server did not recover in the worker");
            mode = "cleanup-fails";
            await source.close();
            await source.close();
        } finally {
            await source.close();
            delete globalThis.location;
        }
        console.log("HTTP deadlines, abort, open/close cleanup, relative worker recovery and real byte-limit restoration passed");
    } finally {
        mode = "cleanup-fails";
        for (const channel of sources) await channel.close();
        for (const response of pending) response.destroy();
        proxy.closeAllConnections();
        await new Promise(resolve => proxy.close(resolve));
    }
}
