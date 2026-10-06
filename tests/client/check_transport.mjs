// Real HTTP regressions: stalled headers/bodies, failed opens and cleanup.
import assert from "node:assert/strict";
import { createServer } from "node:http";
import { JpipChannel } from "../../client/js/jpip_channel.mjs";
import { JpipSource } from "../../client/js/jpip_source.mjs";
import { Worker } from "./worker.mjs";

export async function checkTransport(wasm, server, image) {
    let mode = "pass", entered, stalled;
    const requests = [], assigned = [], closed = [];
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
            if (mode === "headers" && !cid && !url.searchParams.has("model")) {
                pending.add(response);
                entered?.();
                return;
            }
            const upstream = await fetch(server + request.url);
            const cnew = /(?:^|,)cid=([^,]+)/.exec(upstream.headers.get("JPIP-cnew") ?? "");
            if (cnew) assigned.push(cnew[1]);
            const body = new Uint8Array(await upstream.arrayBuffer());
            response.writeHead(upstream.status, Object.fromEntries(upstream.headers));
            if ((mode === "body" && !cid && !url.searchParams.has("model")) ||
                (mode === "open-body" && cnew)) {
                response.write(body.subarray(0, 1));
                pending.add(response);
                entered?.();
            } else if (mode === "refuse-open" && cnew) {
                response.end(new Uint8Array([0]));
            } else if (mode === "unexpected-limit" && url.searchParams.has("model")) {
                assert.deepEqual([...body.slice(-3)], [0, 2, 0]);
                body[body.length - 2] = 4;
                response.end(body);
            } else response.end(body);
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
            // The original and resumed data request time out. The queued call
            // then rejects with the same terminal error, without more HTTP.
            const first = channel.frame(0);
            const next = channel.frame(0);
            await Promise.all([assert.rejects(first, /timed out/), assert.rejects(next, /timed out/)]);
            assert.equal(requests.filter(url => !url.searchParams.has("cclose") &&
                !url.searchParams.has("model")).length, 2);
            assert.equal(requests.filter(url => url.searchParams.has("cnew")).length, 1);
            await channel.close();
        }
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
            await assert.rejects(open());
            assert.equal(assigned.length, count + 1);
            assert.ok(closed.includes(assigned.at(-1)), "failed open leaked its assigned channel");
            const reply = await fetch(`${server}/jpip?cid=${assigned.at(-1)}&stream=0`);
            assert.equal(reply.status, 503, "failed open left the server channel alive");
        }

        mode = "pass";
        const unexpected = await open(); sources.push(unexpected);
        const cid = assigned.at(-1);
        await fetch(`${server}/jpip?cid=${cid}&cclose=${cid}`);
        mode = "unexpected-limit";
        requests.length = 0;
        await assert.rejects(unexpected.frame(0), /did not complete cache restoration/);
        assert.equal(requests.filter(url => url.searchParams.has("model")).length, 1,
                     "unexpected byte-limit response started a continuation loop");
        assert.ok(requests.every(url => !url.searchParams.has("len")));
        mode = "pass";
        await unexpected.close();

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
        assert.ok(closed.includes(assigned.at(-1)), "failed worker open leaked its channel");
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
        console.log("HTTP deadlines, abort, open/close cleanup, relative worker recovery and unexpected EOR passed");
    } finally {
        mode = "cleanup-fails";
        for (const channel of sources) await channel.close();
        for (const response of pending) response.destroy();
        proxy.closeAllConnections();
        await new Promise(resolve => proxy.close(resolve));
    }
}
