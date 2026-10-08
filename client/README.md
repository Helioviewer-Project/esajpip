# client

A JPIP client for the JP2 images and JPX movies that esajpip serves. Given a
frame number and a resolution, it fetches what it does not have yet, decodes
the frame, and returns 8-bit pixels. It also gives each frame's XML metadata
and color table. Data received once is kept, so showing a frame again, or
smaller, makes no request.

| Part | Use it for | Start at |
| --- | --- | --- |
| JavaScript module | A web application | [JavaScript](#javascript) |
| C library | A native program that makes its own HTTP requests | [C](#c) |
| Demonstration page | Seeing it work against your server | [Quick start](#quick-start) |
| `hvc_jpp2j2k` | Turning responses saved with curl into a `.j2k` file | [hvc_jpp2j2k](#hvc_jpp2j2k) |

The C library is the client. The JavaScript module is the same code built for
WebAssembly, with the HTTP exchange and a Web Worker around it.

## Quick start

With the server running on its default port and a `movie.jpx` in its image directory
([`../README.md`](../README.md)):

```sh
cmake -S . -B build-wasm -DCMAKE_TOOLCHAIN_FILE=client/wasm-toolchain.cmake
cmake --build build-wasm --target esajpip_client_wasm
python3 -m http.server -d build-wasm/client/web 8000
```

Then open
<http://localhost:8000/?server=http://localhost:8900&image=movie.jpx>.

The WASM configuration needs [zig](https://ziglang.org/) (`brew install zig`): its
C compiler builds the WebAssembly module, and carries the C library OpenJPEG
needs. WASM builds default to `Release`; set `CMAKE_BUILD_TYPE` to choose
another configuration. Native builds use the usual project configuration.
`build-wasm/client/web/` receives everything a page needs:
`esajpip_client.wasm`, `jpip_source.mjs`, `jpip_worker.mjs`,
`jpip_channel.mjs`, and the demonstration page as `index.html`. Keep
`OpenJPEG-NOTICES.txt` and `dlmalloc-LICENSE.txt` with the distributed output.

To require WebAssembly SIMD for decoding, add `-DESAJPIP_WASM_SIMD=ON` to the
WASM configuration. It defaults to off so the standard module also runs on
engines without SIMD support. A local Node.js 24.19.0 benchmark of a 4096x4096
grayscale cached decode
fell from 247 ms to 228 ms (10 measured decodes after 3 warmups, identical pixel
hashes); performance depends on the engine and image.

On the page:

- The image opens at its lowest resolution. There is a button for each
  resolution, up to `Full size`.
- `Preview (1 layer)` requests one quality layer at the selected resolution.
  Switch to `Full quality` to fetch the remaining layers and redraw. The
  status line reports the quality actually cached, which may already be higher
  than the selection. The opening response has full quality at the lowest
  resolution.
- For a movie, the slider chooses the frame and Play steps through the frames,
  at most 10 a second. While a frame is on its way, only the last slider
  position is kept.
- The strip under the slider shows the cache: dark for frames ready at the
  selected resolution and quality, pale for frames with some cached data.
- "Fetch ahead" fetches the other frames at the selected resolution and quality,
  one at a time, starting after the current frame.
- "XML metadata" shows the XML of the current frame. The status line shows the
  bytes received so far.

The page is an example of the JavaScript module and nothing more. It draws on
a 2D canvas, so it converts the pixels to RGBA and applies the color table
itself.

## Terms

- **Frame.** A JPX compositing layer, numbered from 0; JP2 has one implicit
  layer. `hvc_view.codestream` identifies the codestream used for requests.
  Layers may reorder or share codestreams. Each frame reports its own geometry
  and color table, whether its image is embedded or linked.
- **`reduce`.** The number of highest resolution levels left out, as in
  `opj_decompress -r` and `kdu_expand -reduce`. The size is the full size
  divided by 2^`reduce`, rounded up: a 4096x4096 frame is 1024x1024 at
  `reduce` 2. A value beyond what the frame has gives its lowest resolution.
- **Cached.** A resolution level of a frame is cached once all its data has
  arrived. A request brings only the levels that are not.
- **Channel.** The server's record of what one client has received for one
  image. The server ends a channel left idle for `connections.timeout`, 60
  seconds by default; see [Failures](#failures).

## JavaScript

### Setup

Put `jpip_source.mjs`, `jpip_worker.mjs`, `jpip_channel.mjs` and
`esajpip_client.wasm` in one directory of the site, and import
`jpip_source.mjs`. The page may be on another origin than the server, which
sends `Access-Control-Allow-Origin: *`.

### Showing a frame

```js
import { JpipSource } from "./jpip_source.mjs";

const source = await JpipSource.open({
    wasm: new URL("esajpip_client.wasm", import.meta.url),
    server: "http://localhost:8900",
    image: "movie.jpx" });               // a path below the image directory

const frame = await source.frame(3, { fit: [1024, 768] });

// One byte per pixel: rows are not padded to 4 bytes.
gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, frame.width, frame.height, 0,
              gl.RED, gl.UNSIGNED_BYTE, frame.pixels);

await source.close();
```

`frame` resolves to:

| Field | Meaning |
| --- | --- |
| `index` | The frame number |
| `reduce` | The resolution reduction selected for this frame |
| `width`, `height` | The size of the decoded image |
| `components` | 1 or 3 |
| `pixels` | A `Uint8Array` of `width * height * components` bytes, top row first, components interleaved. It is a copy that belongs to the caller |
| `fullWidth`, `fullHeight` | The size at `reduce` 0 |
| `resolutions` | The number of resolution levels: `reduce` runs from 0 to `resolutions - 1` |
| `layers`, `totalLayers` | The minimum whole quality layers available across the decoded resolutions, and the source's total |
| `quality` | Whole quality layers available per resolution, lowest resolution first. A missing resolution has 0 |
| `complete` | All quality layers are present at the decoded resolution and every lower resolution |
| `ready` | The requested quality is cached at the selected resolution and every lower resolution. Always true after successful `frame` or `fetch` |

`{ fit: [width, height] }` gives the available display area in physical
pixels. The client fits the image into it, preserving aspect ratio, and
chooses the coarsest available resolution that covers the drawn image.
For example, a 4096x4096 image in a 1000x800 area needs an 800x800 image,
so the client decodes at 1024x1024 (`reduce` 2).

The application supplies its CSS display size multiplied by the device
pixel ratio. For zoom, supply the area the whole image would occupy,
including the part outside the viewport. Requests still fetch whole frames.

For explicit control, use `{ reduce: n }`: a nonnegative integer, or
`Infinity` for the lowest resolution. Omitting options gives full resolution.
`fit` and `reduce` cannot be combined. `source.frames` is the number of frames.

### API flow

```text
source = await JpipSource.open(...)
    |
    v
Choose frame index and display area in physical pixels <----+
    |                                                       |
    v                                                       |
frame = await source.frame(index, { fit: [width, height] }) |
    |                                                       |
    v                                                       |
Upload frame.pixels and draw at the desired display size    |
    |                                                       |
    v                                                       |
Finished with source? -- no: wait for next display change --+
    |
    yes
    |
    v
await source.close() when finished with the source
```

`fit` selects the decoding resolution. The application still scales and draws
the returned pixels at its desired display size. No geometry or cache query
is required before calling `frame`.

The client obtains the frame header if missing, chooses a resolution from
that frame's geometry, fetches missing levels, reconstructs the codestream,
and decodes it. This works independently for each frame, including movies
with different frame sizes. Cached levels need no request, but each `frame`
call decodes again.

Optionally, `await source.fetch(index, { fit: [width, height] })` fetches ahead
without decoding. A later `frame` call with the same options uses that cache.

### What the pixels are

| Frame | `components` | `pixels` |
| --- | --- | --- |
| One or two components, no color table | 1 | The first component as gray, scaled to 8 bits |
| Three or more components | 3 | The first three components as RGB, each scaled to 8 bits |
| With a color table | 1 | Indices into the table, unchanged |

Scaling keeps the 8 most significant bits of deeper samples, and shifts
shallower ones up. Signed samples are offset to unsigned first.

A frame with a color table is not converted to color. `palette` gives the
table, and the application applies it, typically as a lookup texture:

```js
const lut = await source.palette(3);     // null if the frame has none
if (lut !== null && lut.channels === 3)
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGB8, lut.entries, 1, 0,
                  gl.RGB, gl.UNSIGNED_BYTE, lut.table);
```

- `table` is a `Uint8Array` of `entries * channels` bytes: for each index from
  0 to `entries - 1`, its `channels` values, scaled to 8 bits. `channels` is 3
  for red, green, blue. It is a copy that belongs to the caller.
- An index can be larger than `entries - 1` when the table has fewer than 256
  entries. The demonstration page uses the last entry for those.
- Indices are supported for a frame of one unsigned component of at most 8
  bits. Any other frame with a color table fails to decode.

The display API does not interpret `colr`, ICC profiles or channel definitions,
and does not convert color spaces such as sYCC to RGB. Three decoded components
are returned in their existing order and displayed as RGB. Use grayscale,
RGB, or supported palette images whose samples already have that meaning.
The server can accept files outside this display subset. The native
`hvc_reconstruct` API preserves the codestream's sample precision and components;
a host using another decoder must handle the file's color interpretation itself.

### XML

```js
const xml = await source.xml(3);         // a string, or null
```

This is the XML that describes the frame; for a FITS image, its header. In a
JPX it is found as `hv_merge` and hvJP2K write it: the `xml` box of the
association box (`asoc`) whose number list (`nlst`) names the frame. In a JP2
it is the first top-level `xml` box.

The XML and the color tables of all frames arrive when the source is opened,
so `xml` and `palette` never make a request.

### Playing a movie

`frame` fetches what is missing before it decodes, so a movie played frame by
frame waits for the server at every new frame. `fetch` gets a frame's data
without decoding it:

```js
for (let index = 0; index < source.frames; index++)
    await source.fetch(index, { fit: [1024, 768] });
```

After that, `frame(index, { fit: [1024, 768] })` decodes from the cache.

- **One at a time.** `frame` and `fetch` run in the order they were called,
  one after the other. `close` aborts the request under way and rejects queued
  calls. With one `fetch` pending, as in the loop above,
  a `frame` call made meanwhile waits for one request at most. A hundred
  `fetch` calls made at once would all run before it.
- **Progress.** `fetch` returns the frame's display status without `pixels`.
  `cached(index, options)` returns the same status without fetching or
  decoding, or `null` if the frame's header is missing. It does not wait for
  requests under way. `ready` says whether the supplied options are satisfied;
  `complete` says whether full quality is cached at the selected resolution.
- **Other resolutions.** A frame cached at `reduce` 1 also serves `reduce` 2
  and above. Fetching it at `reduce` 0 later brings the one missing level.
- **Decoding.** Every `frame` call decodes. Decoded frames are not kept: keep
  the textures of frames shown repeatedly.
- **First request for a frame.** If its main header has not arrived, a
  header-only request (`stream=view.codestream&layers=0`) obtains its size and resolution
  levels before requesting pixels. This adds one round trip on the first
  visit, and makes the request exact even when movie frames differ in size.

### Preview and refine

Add `layers` to either display option to request a lower-quality preview:

```js
const preview = await source.frame(index, { fit: [1024, 768], layers: 1 });
// Upload and display preview.pixels.

const refined = await source.frame(index, { fit: [1024, 768] });
// Replace the texture with refined.pixels.
```

`layers` is a positive safe integer, clamped to the frame's total. Omit it
for all layers. `fetch` accepts the same options, so a movie can be fetched
first with one layer, then refined. The application chooses when to refine
and redraw. Layer count is a quality step, not a percentage of final quality.

The client records whole packets only after the requested window ends
normally. Reconstruction writes those packets and empty packets for missing
layers. A later request adds data to the same bins. Asking for fewer layers
does not discard existing data or lower the quality of the returned frame.
Quality is tracked independently for each resolution, so previewing at a
larger size does not lose the finer quality already fetched at a smaller size.

This uses the server's existing `layers` support and ordinary JPP messages.
Byte-limited previews are not supported yet.

Prefetching also reports quality without decoding:

```js
const options = { fit: [1024, 768], layers: 1 };
const status = await source.fetch(index, options);
// status.ready is true; status.complete may still be false.

const cached = await source.cached(index, options);
if (cached?.ready) {
    const frame = await source.frame(index, options); // decode with no request
}
```

All three methods accept the same options. Status describes that selection:
changing the display size or requested layers can change `ready`. Omitting
options asks about full resolution and full quality.

### Several images

A source is one channel and one Web Worker: fetching and decoding run off the
page's thread, and sources do not wait for each other. Open one per image or
movie on screen. Each module URL, resolved against the page URL, is downloaded
and compiled once per page, whatever the number of sources. Different query
strings identify separate modules.

### Failures

A call that fails rejects its promise with an `Error` whose message says why.

| Failure | What happens | What to do |
| --- | --- | --- |
| `index` is not a frame, or the display options are not valid | `RangeError`; no request is made | Fix the call |
| A transport interruption, or the server reports that the channel has ended or does not exist | Restore a replacement channel from the retained cache and retry the request once | No application action if recovery succeeds |
| Recovery fails without an ingestion refusal, or its resumed request loses transport | The call rejects and retains the cache. A later call needing data tries a fresh replacement channel | Retry the call when service returns |
| Another HTTP error occurs | The call rejects. Later calls needing a request reject with the same error; valid cached frames remain usable | Close the source and open the image again |
| JPP ingestion refuses a response | The cache is retired. Frame, cache, XML and palette calls report the refusal | Close the source and open the image again |
| "the server did not complete the requested quality layers" | The call rejects because the response ended without completing the requested quality | |
| A call after `close()` | Rejects with "the source is closed" | |

If loading or compiling the WASM module fails, a later `JpipSource.open` with
the same module URL tries again. A successfully compiled module is shared by
sources on the page.

`timeout` limits silence while waiting for headers or the next body chunk; a
response that continues arriving can run longer. Pass it in milliseconds to
`JpipSource.open` (default 60000), or as the fourth argument of `JpipChannel.open`:
`{ timeout: 60000 }`. A timeout rejects the current call without an immediate
retry. The next call needing data attempts recovery with the retained cache.
Old-channel cleanup runs independently and cannot add another wait to recovery.
Shared WASM module downloads still have a 60-second total deadline; direct channel
module downloads use the configured timeout as a total deadline.
`close()` aborts the active JPIP exchange, releases the local cache, and attempts
server cleanup within the configured idle timeout; cleanup failure does not
reject `close()`. A failed open awaits bounded cleanup of any assigned channel
before ending its worker and rejecting.

`server` may be relative to the page URL. `image` is an unescaped path below the
server's image directory; the client escapes each filename component.

An idle channel expires after `connections.timeout` (60 seconds by default).
The next request for uncached data restores it automatically. A server restart
is handled the same way. Metadata, prepared frame headers, preview quality
records and precinct data-bins remain in the client. Cache hits need no live channel.

Recovery opens the original target with no frame selected and declares retained
complete bins and exact partial byte prefixes in bounded `model` batches. It
never declares partial `M0`. Metadata repeated during restoration is checked
against the retained bytes. A byte-limit EOR continues that restoration window
without repeating its model declarations. Empty limited replies are rejected;
continuations per batch are capped by the number of bytes already received, an
upper bound on replayable metadata. Normal responses also accept identical overlap
and append only new bytes to each bin. Once restoration finishes, the interrupted
request resumes. A transport failure ends that attempt; a later uncached call
starts restoration again. Transport failure leaves valid cached frames usable. A refused
JPP or restoration response retires the cache; even previously ready frames
cannot be fetched from it. Pixels already returned to JavaScript remain owned
by the caller. Closing a source prevents it from reopening a channel.

This relies on the server's immutable-source contract: the target and its linked
files must remain unchanged across channel replacement, including server restarts.
`JPIP-tid: 0` does not establish file identity. If the target changes, close the
source and open it again. Busy channels, invalid requests, missing targets, and
internal server errors are not retried.

### Memory

A source keeps every byte it receives until `close()`: the server does not
send data twice on a channel, and takes no note of a client that drops some. A
movie fetched whole at full size retains its codestream payload, allocation
headers and alignment, and the tables that index the data-bins. Partial bins
reserve room geometrically until completed. The WASM build links WASI libc's
`dlmalloc`, avoiding Zig's power-of-two allocation rounding. System growth uses
chunks between one eighth and one quarter of current linear memory, with a
64 KiB minimum, capped by remaining wasm32 address space, to limit repeated
full-memory copies on engines that cannot
reserve the address space. This spare capacity is separate from block sizes.

A response is accumulated in one geometrically grown JavaScript buffer, whose
capacity is less than twice its body size. Growth briefly holds both the old
and new buffers, plus the incoming network chunk. Ingestion copies the body
into a temporary WASM allocation, then copies its data-bin payload into the
cache. The temporary allocation is freed after ingestion, including when the
WASM call throws. The JavaScript buffer is outside linear memory; the staging
allocation and cache compete for wasm32's 4 GiB address space.

Decoding adds, for its duration, a copy of the frame's codestream and OpenJPEG's working memory.
The module keeps the last decoded image until the next decode. The page gets
its own copy. A frame's parsed geometry and prepared reconstruction header are also
kept once first used, until the source closes. Linear memory grows to accommodate
peak demand and does not shrink when allocations are freed; the allocator can
reuse that space. A worker's memory is released when the source closes and the
worker terminates.

### Reference

| `JpipSource` | Result | Requests |
| --- | --- | --- |
| `JpipSource.open({ wasm, server, image, timeout })` | A source. `wasm` is the URL of the module, `server` the server's address, `image` a path below its image directory | 1 |
| `frames` | The number of frames | |
| `received` | Bytes of response bodies so far, as of the last `frame` or `fetch` | |
| `frame(index, options = {})` | The decoded frame. Options: `{ fit: [width, height] }` or `{ reduce: n }`, optionally with `layers` | For the resolution and quality levels not cached |
| `fetch(index, options = {})` | The frame's display status without `pixels`; same options as `frame` | For the resolution and quality levels not cached |
| `cached(index, options = {})` | Display status for those options, or `null` if the header is missing | 0 |
| `xml(index)` | A string, or `null` | 0 |
| `palette(index)` | `{ entries, channels, table }`, or `null` | 0 |
| `close()` | Closes the channel, frees the cache and ends the worker | 1 |

All but `frames` and `received` return promises.

The first request brings the metadata of all frames and the lowest resolution
of frame 0.

### Without a worker

`jpip_channel.mjs` exports `JpipChannel`, which is what the worker runs. Use
it directly where the code is already off the main thread, or under Node.js:

```js
import { JpipChannel } from "./jpip_channel.mjs";

const channel = await JpipChannel.open(wasm, server, image);
```

`wasm` is a compiled `WebAssembly.Module`, the module's bytes, or its URL. The
methods are those of `JpipSource`, except that `cached`, `xml` and `palette`
return their result directly, not a promise.

## C

`esajpip_client` is a static library; its headers are in `client/`. Inside
this build, `target_link_libraries(app PRIVATE esajpip_client)` is enough. The
library does no I/O: the program sends the requests and passes each response
body to the library. It reconstructs codestreams for the host decoder and
does not depend on OpenJPEG. Native decoding tests link `esajpip_client_wasm`,
which compiles the extended source list also used by the WASM module.

To build the native client without server dependencies:

```sh
cmake -S . -B build-client -DESAJPIP_CLIENT_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-client --target esajpip_client --parallel
```

Run these commands from the repository root. This configuration skips the
server, CLI tools and served-profile filesystem checks; it requires no pkg-config, GLib, llhttp or zlib. Set
`-DCMAKE_POSITION_INDEPENDENT_CODE=ON` when linking the static libraries into
a shared library.

### Source API

Use `hvc.h` for one source; it exposes no cache or reconstruction implementation
headers. The client owns the data-bin cache, metadata and
pending frame request. The same API drives the WebAssembly client.

```text
Create source
    ↓
Host opens channel and submits the initial response
    ↓
Read frame count, XML and palette
    ↓
Prepare frame with viewport fit or reduction, and quality
    ├─ HEADER → host requests stream=view.codestream,layers=0
    ├─ FRAME  → host requests stream=view.codestream,fsiz=width,height,closest,layers=k
    │               ↓
    │          Submit response, then prepare again
    └─ READY  → reconstruct codestream → host decodes and displays
    ↓
Host closes channel; destroy source
```

`hvc_options` initialized to zero selects full resolution and quality.
Set both `fit_width` and `fit_height` to the physical viewport dimensions to
select a resolution for an aspect-ratio-preserving fit, or set `reduce` to
remove highest resolution levels. `INT_MAX` selects the lowest resolution.
Do not combine a fit with a nonzero reduction. `layers=0` means all source
layers; positive values are clamped to the frame's layer count.

`hvc_palette` returns the selected layer's display table: one grayscale or
three RGB bytes per entry, with CMAP columns and CDEF color order resolved.
Its natural entry count is preserved. Table conversion and absence are cached
per layer, since layers sharing a codestream can have different channel order.
This is a metadata query and needs no codestream header; the decoder still
checks index components and precision. A short output buffer is untouched and
returns the required entry count. `hv_render_palette` provides the same conversion
from resolved rendering instructions for host decoders, allowing an explicit
table length and repeating the last entry beyond the original palette.

`hvc_status` inspects readiness without preparing a request.
`hvc_prepare` also remembers the exact request to confirm after its
response. A missing header produces `HVC_HEADER`; its geometry is zero.
Once the header is cached, `HVC_FRAME` supplies exact dimensions and
`requested_layers`. `HVC_READY` needs no request. `view.source` describes
the original components and geometry; `view.layers` is the minimum cached
quality across the selected resolution levels.

The host retains HTTP, channel routing, scheduling and decoding. Submit the
opening response with `hvc_response`, then use `hvc_frames` to
read the frame count. Keep one request outstanding per source, serialize all
calls, and request whole-frame windows. A byte limit is allowed; after a limit
EOR, prepare again and use the returned dimensions and `requested_layers`.
Preparation can raise quality to full when previously received unconfirmed
bytes make a smaller layer boundary ambiguous. Normal completion confirms
whole-packet boundaries. The client uses each frame's own header; old servers
still have the mixed-size movie limitation documented in [CLASSICAL.md](CLASSICAL.md).

For exact continuation progress, use `hvc_response_progress` instead of
`hvc_response`. Pass a stable window token for each sequence of limited responses
(a frame index is convenient, `UINT64_MAX` identifies metadata). Its additional
output reports new data-bin ranges or final flags delivered within that window.
On a limit EOR, zero progress means the server is repeating data and the host
must stop that continuation. A completed window retires only its own history.
Tracking is optional, stores no payload copies, and does not expand
`hvc_prepare`'s single-pending-request contract. Full-quality hosts can track
independent pipelined windows while using status rather than prepare.

Call `hvc_new_channel` when replacing the channel, retaining cached data while
resetting delivery histories. A replay of cached bytes on the new channel is
new delivery, and a repeated replay within one continuation is not. During
metadata restoration, `hvc_restore_response_progress` applies the existing
immutable-metadata checks and reports progress using the metadata window.
JHV and the JavaScript client use this same tracker; HTTP, retries and scheduling
remain with the hosts. The JavaScript client continues byte- and response-limited
frame windows automatically and stops on the first continuation with no new
delivery. Deploy its JavaScript and WebAssembly builds together; the response
export now also accepts the window token.

```c
hvc *source = hvc_create(inspect, decoder_context);
if (source == NULL)
    return fail("out of memory");

/* The host supplies the initial response body. */
if (hvc_response(source, body, body_size) < 0 || !hvc_frames(source))
    goto failed;

hvc_options options = {0};
options.fit_width = viewport_width;
options.fit_height = viewport_height;
options.layers = 1;
hvc_view view;
for (;;) {
    if (hvc_prepare(source, frame, &options, &view) != 0)
        goto failed;
    if (view.request == HVC_READY)
        break;
    /* Host sends HEADER or FRAME fields described above, obtains body/size. */
    if (hvc_response(source, body, body_size) < 0)
        goto failed;
}
hvc_input *input = hvc_input_open(source, frame, view.reduce);
if (!input)
    goto failed;
/* Decode with hvc_input_size and hvc_input_read, then close the decoder. */
hvc_input_close(input);
hvc_destroy(source);
return 0;

failed:
/* Report hvc_error(source), close the host's channel, then destroy. */
hvc_destroy(source);
return -1;
```

For transport interruption before ingestion refusal, keep the source for channel recovery. `hvc_model` writes retained-bin
declarations in bounded batches; start its cursor at zero and stop at an empty
batch. Select no codestreams (`stream` equal to `hvc_codestreams(source)`, `layers=0`).
The first batch opens a replacement channel on the original target; later
batches use its `cid`. Submit these responses with `hvc_restore_response`,
which checks repeated metadata without changing cached bytes. On
`BYTE_LIMIT_REACHED`, continue the same window without its `model` field until
`WINDOW_DONE` or `IMAGE_DONE`. Send each model batch only once: partial-bin
amounts are additive on older servers. Then retry the original pending request. Restoration retains geometry, quality and metadata
pointers and does not complete the pending frame request. Use it only after a
successful initial metadata exchange, for the same immutable target.

A byte-limit EOR retains bytes without confirming quality; the host can prepare
another request. A refused response may have applied preceding messages. Discard
the client and channel on refusal, including restoration refusal, and invalidate
its persisted cache. The library does not automatically reject later calls.
Reusing that source could mix bytes from changed targets. Transport interruption
without an ingestion refusal can instead use the recovery procedure above.

Destroying the source releases its metadata and cache. `hvc_xml` returns borrowed
bytes valid until destruction; `hvc_palette` copies into a host buffer.
`hvc_codestream` preserves sample precision and follows the same size-query and
caller-buffer convention as `hvc_reconstruct`.

For a disk cache, `hvc_export` writes one frame's main header and complete
data-bins under the same convention, and `hvc_import` adds such a block to a
frame of any JPIP source showing the same image, whatever its position there.
The block is a sequence of JPP messages without an end-of-response message.
Unfinished bins are left out, so export after a completed response. After
import, `hvc_status` reports what is ready; a server that sends those bins
again is accepted as an identical replay. A malformed block, or one differing
from bytes the source already holds for that frame, is refused without changing
the source: drop that cache entry and request the frame. A block imported into
a frame with no bytes held cannot be checked. The host's key must identify the
image, and a later response that conflicts with it is refused as described above.

### Requests

One channel per image, one request at a time on it.

```
GET /movie.jpx?cnew=http&type=jpp-stream&stream=3&fsiz=1024,1024,closest
GET /<path>?cid=<cid>&stream=3&fsiz=2048,2048,closest
GET /<path>?cid=<cid>&cclose=<cid>
```

- The first request names the image and asks for a channel. Its response has
  the header `JPIP-cnew`, whose `cid` and `path` fields the later requests
  use.
- `stream` is `hvc_view.codestream`, the codestream mapped to the JPX layer. `fsiz` is its size at the `reduce` wanted: the full
  size divided by 2^`reduce`, rounded up.
- The first response of a channel also carries the metadata of all frames,
  whatever it asks for.
- A frame's size and resolution levels are known once its main header has
  arrived. Before that, request `stream=<view.codestream>&layers=0`, then use
  `hvc_reconstruct_status` to size the pixel request.
- Full-quality pixel requests may use `len`. Consume every response through
  EOR and continue after a byte-limit EOR. A host using only `hvc_response` and
  `hvc_status` obtains readiness from completed bins. A prepared host must use
  each returned `hvc_view`, including `requested_layers`. Do not repeatedly retry
  a normally completed window that leaves its requested view unready.
- For layer-limited windows, prefer `hvc_prepare`/`hvc_response`: they confirm
  quality only after normal window completion. If a prior response left
  unconfirmed bytes, preparation raises the request to full quality to avoid
  treating a mid-packet byte offset as a layer boundary. At the low level,
  `hvc_reconstruct_confirm` requires that unfinished bins had no unconfirmed
  tail before the layer-limited request. Unconfirmed bytes remain cached but
  are omitted from decoder input. Successful reconstruction is not readiness.
- [`../JPIP_PROFILE.md`](../JPIP_PROFILE.md) describes the requests the server
  accepts, and [`../CHANNELS.md`](../CHANNELS.md) its channels, status codes
  and timeouts.

### Low-level calls

| Call | Header | Result |
| --- | --- | --- |
| `hvc_cache_begin`, `hvc_cache_release` | `hvc_cache.h` | The store of one source |
| `hvc_jpp_begin`, `hvc_jpp_next`, `hvc_jpp_reason` | `hvc_jpp.h` | The messages of a response body, and why the response ended |
| `hvc_cache_apply` | `hvc_cache.h` | A message added to the store |
| `hvc_cache_model` | `hvc_cache.h` | Retained bins declared in bounded batches for a replacement channel |
| `hvc_cache_match_metadata` | `hvc_cache.h` | Repeated metadata checked against complete retained bins during restoration |
| `hvc_reconstruct_confirm` | `hvc_reconstruct.h` | Whole-packet prefixes recorded after a completed layer-limited window |
| `hvc_metadata_open` | `hvc_metadata.h` | XML associations indexed using the existing presentation's codestream and layer counts |
| `hv_metadata_close` | `jpeg2000/hv_metadata.h` | Releases the metadata index |
| `hv_metadata_codestream_xml` | `jpeg2000/hv_metadata.h` | A codestream's XML, with file-level fallback |
| `hv_metadata_layer_xml` | `jpeg2000/hv_metadata.h` | A layer's XML, including its registered codestreams and file-level fallback |
| `hv_render_read`, `hv_render_palette` | `jpeg2000/hv_render.h` | Resolved layer channels and their display palette |
| `hvc_reconstruct_status` | `hvc_reconstruct.h` | A frame's size, components and resolution levels, and how many levels are cached |
| `hvc_reconstruct` | `hvc_reconstruct.h` | A frame as a JPEG 2000 codestream |
| `hvc_openjpeg_decode` | `hvc_openjpeg.h` | OpenJPEG decoding in `esajpip_client_wasm` |

The low-level APIs remain available for programs that need direct message or
data-bin access. Each header documents its calls' results. Calls that take `error` and
`error_size` write a message there when they fail.

### Low-level notes

- **`hvc_frame_status`.** `complete` counts cached resolution levels from the lowest,
  so the lowest usable `reduce` is `resolutions - complete`, and 0 means
  nothing can be decoded yet. All fields are 0 until the frame's main header
  has arrived. The first call prepares the geometry and reconstruction
  header; later calls reuse them and look the precinct bins up. It reads no
  packet and decodes nothing.
- **`hvc_reconstruct`.** Called with no buffer, it returns the size; called
  with one, it writes. The codestream has empty packets above the cached
  levels or quality layers, so any decoder reads it. For full-quality requests,
  leave out `resolutions - complete` levels. For confirmed previews, use the
  reduction requested. Unconfirmed packets are replaced with empty packets; confirmed
  prefixes stay usable if a later refinement appends unfinished packets.
- **`hvc_openjpeg_decode`.** `HVC_IMAGE_SAMPLES` scales samples to 8 bits.
  `HVC_IMAGE_INDICES` keeps them as they are, for a frame with a color table;
  it accepts one unsigned component of at most 8 bits. See [What the pixels
  are](#what-the-pixels-are).
- **`hv_metadata`.** Start with `{0}`. `hvc_metadata_open` fails while the
  metadata is incomplete; a normally ended opening response need not contain
  all metadata on an older server. The
  index points into the store: XML pointers stay valid until
  `hvc_cache_release`, and the index must be closed before the store is
  released.
- **Client color tables.** `HVC_PALETTE_MAX` bytes hold any table returned
  by `hvc_palette`. A call with capacity 0 returns `entries` and sets
  `channels` without writing.
- **Errors in a response.** When `hvc_jpp_next` or `hvc_cache_apply` fails, the
  response is refused. The source API does not latch a failed state. The host
  must retire that source and channel and invalidate its persisted cache. A new
  source is required for retry; a later response must not be merged into a
  potentially mixed cache. Already captured immutable inputs retain their own
  bytes, subject to the source-lifetime requirements in `hvc_decode.h`.
- **Threads.** A store has no locking: use it from one thread at a time.
- **Pixels.** `image.pixels` is allocated with `malloc`; the caller frees it.

`hvc_wasm.c` and `js/jpip_channel.mjs` together provide a complete host example.

## Limits

- **Whole-frame windows only.** No requested regions. Byte-limited responses
  accumulate in the cache; display only reductions/quality reported ready.
  Serialize source operations, including input creation, with response ingestion.
  An existing immutable decode input can be read while later responses arrive.
- **No eviction.** The cache grows until the source is destroyed.
- **Bounded recovery.** An operation attempts one replacement channel. There
  is no background keepalive or retry loop while a server remains unavailable.
- **This server's files.** The client reads what esajpip sends for the files
  it accepts ([`../JPIP_PROFILE.md`](../JPIP_PROFILE.md)): one tile, no
  component subsampling. The [display API](#what-the-pixels-are) has narrower
  precision, component and color support. It is not a general JPIP client.
- **Lossy images.** Pixels from the WebAssembly module and from a native build
  can differ by 1, as the two round the floating-point wavelet differently.

## hvc_jpp2j2k

```
hvc_jpp2j2k [-c codestream] -o file.j2k response [response ...]
```

Writes a frame as a `.j2k` file from the response bodies of one channel, in
the order received. With `image.jp2` (4096x4096 here) on the server:

```sh
# The whole image at 1024x1024, on a new channel.
curl -s -D headers.txt -o r1.jpp \
  'http://localhost:8900/image.jp2?cnew=http&type=jpp-stream&stream=0&fsiz=1024,1024,closest'
build/client/hvc_jpp2j2k -o frame1024.j2k r1.jpp
opj_decompress -i frame1024.j2k -o frame1024.pgm -r 2

# Then at full size on the same channel: only what is missing is sent.
cid=$(sed -n 's/^JPIP-cnew: cid=\([0-9a-f]*\).*/\1/p' headers.txt)
curl -s -o r2.jpp "http://localhost:8900/jpip?cid=$cid&stream=0&fsiz=4096,4096,closest"
build/client/hvc_jpp2j2k -o frame4096.j2k r1.jpp r2.jpp
opj_decompress -i frame4096.j2k -o frame4096.pgm
curl -s "http://localhost:8900/jpip?cid=$cid&cclose=$cid"
```

`-r 2` leaves out the two resolution levels that were not requested. The
decoded pixels equal those of `image.jp2` decoded with the same options.

For a frame of a JPX, `-c` names it:

```sh
curl -s -o f3.jpp \
  'http://localhost:8900/movie.jpx?cnew=http&type=jpp-stream&stream=3&fsiz=1024,1024,closest'
build/client/hvc_jpp2j2k -c 3 -o frame3.j2k f3.jpp
```

The final response must complete its window and the selected codestream must
have a whole full-quality resolution, with no nonempty precinct bins outside
that complete resolution prefix. Header-only and unconfirmed layer-limited
responses are refused. Byte-limited responses can be combined through completion.
Refusal leaves any existing output file unchanged.

The tool prints the number of messages of each response and why it ended. It
exits with 0 on success, 1 on an error and 2 on a usage error.

## Local and JPIP decoder access

`hvc_open_local(path, inspect, context, error, capacity)` and
`hvc_create(inspect, context)` use the same frame and decoder interfaces.
Include `hvc.h` for sources and requests, and `hvc_decode.h` for decoder input
and presentation. The core library has no decoder dependency.

A frame is a JPX layer in file order, or JP2's single implicit layer. The library
resolves its registered codestream for bytes and palettes. XML considers the
layer and its registered codestreams in box order, with file-level fallback.
`hvc_view.codestream` supplies the JPIP request ID independently of the frame
number. Restoration uses `hvc_codestreams`, independently of the layer count.
Multiple-codestream composition is explicitly unsupported for decoding.

The inspector receives the same `hvc *` and frame index for either source.
It opens decoder input, calls `hvc_render_read` with the decoder's output
component count, and checks that its decoder supports those channel instructions.
It returns exact dimensions at each reduction and the usable resolution/quality
counts. Successful inspection is cached per frame; failures can be retried.
`hvc_info_read` returns this immutable geometry without scanning readiness.
It requires an inspector and an available frame header.
Remote inspection waits for the main header. Its geometry must agree with the
restricted JPIP coding profile, which supplies request geometry and cache quality.
Local `hvc_prepare` returns READY immediately, so the same request loop works
for either source. A NULL inspector uses that profile for JPIP status; local status requires an
inspector. Metadata and byte access need no inspector.

`hvc_render_read` supplies the same channel instructions and exact palette
samples for either source. Local headers are retained in owned memory; the JPIP adapter reads
inline headers and validates codestream placeholders without modifying metadata.
Presentation headers partitioned into separate data-bins remain unsupported.

`hvc_input_open(source, frame, reduce)` returns an immutable native decoder
input. Local reads use buffered files, including fragmented codestreams. JP2 files close
after metadata loading and each inspection/decode. The main JPX file remains
open; seek/read operations are synchronized. Each input owns at most one
external fragment handle, opened on demand and closed with that input. JPIP
reconstructs in one pass into owned native memory, copying only resolutions
needed by `reduce`. Higher-resolution packets are empty, preserving the original
header and exact decoder grids. Subsequent reads and seeks never reconstruct
again. Use `reduce=0` for all resolutions and `INT_MAX` for header inspection.
Receiving more data does not change an open input. Close the decoder and input
before destroying the client. Input reads can run independently of serialized
client calls. A decoder using `hvc_input_read` needs no source-specific branch.
`hvc_input_data` offers an optional contiguous view for WASM's OpenJPEG adapter.
`hvc_codestream` remains a convenience for callers wanting their own byte copy.

## Tests

```sh
ctest --test-dir build -L client --output-on-failure
```

The tests are in `../tests/client/` and need no network.

The native client and vendored OpenJPEG are both instrumented with
`ESAJPIP_SANITIZE=ON`. Build the client targets and run their tests with:

```sh
ESAJPIP_TEST_TARGETS='client_tests replay' tests/run.sh sanitize -L '^(client|client_fuzz)$'
```

The existing extended profile also supplies integer/conversion checks.
Linux Valgrind runs include all client tests and both client seed replays:
`sh tests/run_linux_docker.sh valgrind`. They preserve memory-check reports.

The response and source fuzz targets use real server-generated seeds and the
same harnesses for mutation and deterministic replay. See
[`../tests/fuzz/README.md`](../tests/fuzz/README.md#client) for their input
contracts, bounds and focused run commands. These native diagnostics cover the
C code shared with WASM; JavaScript transport and workers still need the live
checks below.

| Test | What it checks |
| --- | --- |
| `client_cache` | The store: appending, identical overlap/replay, conflict and final-size refusals, completion, many bins and cache-model batching |
| `client_jpp` | Messages written by the server's own writer and by the client's, read back |
| `client_jpp_malformed` | Damaged messages, all refused |
| `client_reconstruct` | Every codestream of the corpus, the transcoder's reference images and the merger's reference movie, served by the server's own code at each resolution: the written codestream, the cached levels, the decoded pixels, and the movie's frame count and XML |
| `client_image` | Sample scaling at several precisions, and unchanged color table indices |
| `client_source` | Local/JPIP equivalence for JP2 and JPX with reordered layers, fewer layers than codestreams, channel/palette instructions, decoder geometry, immutable inputs and pixels at every reduction; also header requests, per-frame geometry and viewport fit, preview confirmation, refinement, rejected responses, pending-request preservation during restoration, metadata replay, first inspection with partial data, byte-limit to layer-limit transitions and decoded pixel equality; frames exported and imported between sources, their replay by the server, and refused blocks leaving the source unchanged |
| `client_converter` | Refusal of incomplete/header-only/unconfirmed quality input without overwriting output; completed continuation, reduced windows and replay |

When Zig and Node.js are available, the normal native test build also builds
the scalar and SIMD WASM modules and registers `client_wasm` and
`client_wasm_simd` with CTest. Each test starts private
local servers and runs the JavaScript, worker, layer-mapping and recovery checks
on the checked-in RGB, non-square and movie fixtures. It asserts that response
bodies, XML and decoded pixels each occupy addresses above 2 GiB, and checks
bounded growth in nine live instances without block rounding, progress timeouts,
retryable recovery, worker import failure, the module import/export contract,
closed-client entry points, bounded string/quality reads, duplicate worker replies,
clone and receive-buffer allocation failures, real byte-limit restoration,
decode failure and allocation cleanup after a trap,
and the lossless synthetic RGB pixels against their source formula. Set `ESAJPIP_NODE` to the
Node executable at configuration time if it is not on PATH. Without Zig or Node,
both tests remain registered and CTest reports them as skipped with the reason.

The JavaScript can also be checked against a running server:

```sh
node tests/client/check.mjs build-wasm/client/web/esajpip_client.wasm \
  http://localhost:8900 movie.jpx
```

It checks argument validation, error types, cache reuse and closing, through
`JpipChannel` and through `JpipSource` with its worker. It decodes the first
frame at every resolution and the others at their lowest, and prints for each
the size, the bytes received, a checksum of the pixels, and the sizes of the
XML and the color table. The printed checksums are for comparing runs. The
automated `client_wasm` test separately checks the synthetic fixture against its
known pixel values.

Layer mapping has a separate live WASM check. Generate the one-layer and
swapped-layer JPX fixtures in the test server's image directory:

```sh
build/tests/client/test_client_source --write-fixtures /path/to/images
node tests/client/check_layers.mjs build-wasm/client/web/esajpip_client.wasm http://localhost:8900
```

Recovery has a separate live check that starts and stops its own server with a
short idle timeout:

```sh
node tests/client/check_recovery.mjs build-wasm/client/web/esajpip_client.wasm \
  build/esajpip /path/to/images movie.jpx other-movie.jpx
```

It checks idle expiry, server restart, interrupted response bodies, retained
preview pixels and metadata, refinement against an uninterrupted transfer,
request-line limits, bounded failures, terminal errors, and close during a
pending request. Through `JpipSource` and its worker it also checks failed
module-load retries, concurrent independent movies, expiry, restart and close
during recovery. The second image is optional; omit it to open two independent
sources for the same image. Use images with several resolution levels and
quality layers. A large preview also exercises several cache-model batches.
Restoration declares the retained bins sequentially, using the space available
within the server's 2 KiB request-line limit. Metadata not declared in the first
batch is replayed and compared with the cache to detect a changed target.

## How it works

The server delivers a codestream as data-bins: one for the main header, and
one per precinct holding that precinct's packets. The client keeps them in a
hash table (`hvc_cache.c`). To decode a frame it writes them back as a
codestream (`hvc_reconstruct.c`): the main header, one tile-part, and the
precinct bins in the order of their identifiers, which is resolution,
position, component (RPCL). A precinct with no data gets one empty packet per
layer. No packet is parsed.

The file's boxes arrive as metadata bins (`hvc_metadata.c`): bin 0 holds the
top-level boxes, with a placeholder for each codestream and, in a JPX, for
each association box, whose contents are a bin of their own. A color table is
the palette box (`pclr`) that the component mapping (`cmap`) applies, from the
JP2 Header box or, in a JPX, from the frame's own codestream header box
(`jpch`) where that has one.

| File | Responsibility |
| --- | --- |
| `hvc.c` | Source ownership, response ingestion, frame request planning and automatic quality confirmation, shared by native callers and WASM |
| `hvc_jpp.c` | JPP-stream messages (T.808 A.2 and D.3) |
| `hvc_cache.c` | One source's data-bins, retained across channel replacement |
| `hvc_frame.c` | Geometry and reconstruction header prepared once per frame, owned by its main-header bin |
| `hvc_reconstruct.c` | One codestream of the store as a JPEG 2000 codestream; its cached resolution levels |
| `hvc_metadata.c` | Metadata-bin lookup and JPIP placeholder resolution for the shared metadata reader |
| `hvc_openjpeg.c` | Decoding to 8-bit pixels, with OpenJPEG |
| `hvc_jpp2j2k.c` | Writes a selected codestream from saved JPP response bodies |
| `hvc_wasm.c` | The WebAssembly module's entry points |
| `js/jpip_source.mjs` | `JpipSource` |
| `js/jpip_worker.mjs` | The Web Worker of a source |
| `js/jpip_channel.mjs` | `JpipChannel`: the HTTP exchange around the module |
| `demo/index.html` | The demonstration page |
| [`vendor/openjpeg/`](vendor/openjpeg/README.md) | OpenJPEG 2.5.4 |
