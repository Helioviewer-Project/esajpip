# Native hosts using classical esajpip servers

The C client does not make HTTP requests. A native host such as JHelioviewer
can keep a classical request policy around the same `hvc` cache and
reconstruction API. The JavaScript transport uses the new server's policy.

Inline metadata, grouping and association boxes, and metadata reached through
placeholders are standard JPX/JPIP representations. Their parsing is general
client functionality and must remain when older servers are retired.

## Request policy

Keep one persistent HTTP connection for the channel and serialize requests.
The classical server associates its channel with that connection. Include
`len` on every data request. Unlike the new server, the classical response
generator has no unlimited default budget.

Open with `cnew=http&type=jpp-stream&tid=0&len=512`. Parse `JPIP-cnew`, then
request `cid=<cid>&stream=0&metareq=[*]!!&len=2000000` until metadata is
complete. Submit each response with `hvc_response` before reading the
frame count or XML.

Use `hvc_options.layers=0`, meaning full quality. Classical servers do
not implement layer selection. A missing-header request is
`cid=<cid>&stream=<view.codestream>&len=<budget>`, without window fields.
For a prepared frame, use W,H from that frame's `hvc_view`, at its
requested resolution. Do not reuse another frame's geometry or approximate
fit dimensions. Request:

```text
cid=<cid>&stream=<view.codestream>&fsiz=W,H,closest&rsiz=W+1,H+1&roff=0,0&len=<budget>
```

Evaluate W+1 and H+1 numerically. The one-pixel padding compensates for the
classical precinct-selection calculation, which can omit the last precinct
when the last image coordinate is exactly a precinct boundary. The new
server clips this window to the image. With exact frame dimensions, padding
does not change the resolution chosen by `fsiz` or the decoding geometry.
With approximate dimensions, the older server can scale the padded region
past the image and fail during packet lookup. Do not pad a priming request
before the frame's own geometry is known.

Response continuation is general client behavior. A full-quality host may use
`hvc_response` and `hvc_status` without preparing requests. Alternatively, use
`hvc_prepare` after each response. In either case, read through EOR: byte-limit
EOR retains bytes and allows continuation on the same channel. Readiness is
not a substitute for consuming the response. A normally completed window
(`WINDOW_DONE` or `IMAGE_DONE`) that leaves the requested view unready is a
failure, not a reason to repeat that completed window indefinitely. Reconstruct
for display only after the requested view is ready. Full-quality requests may
be interleaved between frames; keep each frame's own geometry. JHV's frame
budget is 2 MiB.

## Replacing a channel

Keep the client for the same immutable target. The classical server needs a
fresh persistent connection and channel. Do not use the new-server recipe
`stream=<codestream count>` to select no codestreams on a classical server.

After successful initialization, codestream 0's main and empty tile headers are
cached. Start a `hvc_model` cursor at zero and send each returned batch
with:

```text
stream=0&model=M0,[0]Hm,[0]H0,<batch>&len=2000000
```

Add `cnew=http&type=jpp-stream&tid=0` and the original target for the first
batch, then the returned `cid` for later batches. Omit window fields. The
explicit complete root-metadata and header declarations suppress unsolicited
replay before their descriptors appear in later model batches. The metadata
initialization loop must have completed before declaring `M0`; otherwise the
server would withhold missing metadata. This also prevents a large classical
metadata bin from exhausting the restoration response budget. Keep `len` after
`model`: the classical parser can lose a terminal descriptor at query EOF.
The model's partial precinct amounts are additive, so send each batch once.
The classical HTTP parser truncates the complete request URI to 1,023 bytes.
Size each model buffer for the space left after the path, query fields, separators
and trailing `len`. An oversized URI can lose `len` and return an empty HTTP body
without EOR. This was reproduced against live ROB; do not accept that empty body
as a successful restoration.

The current new server limits the complete request line to 2,048 bytes and the
complete request head to 4,096 bytes (`server/http/request_head.cc`). For
`GET <URI> HTTP/1.1\r\n`, that leaves 2,033 URI bytes. It rejects oversized
requests with HTTP 431 rather than truncating them. When classical support is
retired, recheck those limits and update JHV's `JPIPSocket.MAX_URI` together
with `extra/test/j2k/JPIPSocketTest.testModelBudget`. Keep exact batch sizing
and the outgoing length check. JHV also carries these retirement instructions
next to the constant, so removal does not depend on finding this document.

A restoration response can exhaust `len` on movies with many separate metadata
bins even when `M0` is declared complete. On `BYTE_LIMIT_REACHED`, continue with
`cid=<cid>&stream=0&len=2000000` until `WINDOW_DONE` or `IMAGE_DONE`. Do not repeat
`model`: partial-bin amounts would be added twice. Finish this replay before
sending the next model batch or resuming image requests.

Pass each restoration response, including continuations, to `hvc_restore_response`; it validates
any repeated metadata against the retained bytes without modifying the cache
or completing a pending frame request. Stop at an empty model batch, then
resume the interrupted window. This procedure requires complete initial
metadata and cached codestream-0 headers, not just a successful channel opening.

## Retirement and limits

Only the older-server host request and channel-restoration policy is removable.
Keep general metadata parsing and its regression tests. The restoration recipe
above is specific to the tested server revision and the client's immutable-target,
whole-frame movie profile, not a general JPIP recovery procedure.

Response-generator validation used classical revision
`35aca93e6f6b1f3ec203035c9a872d0fd5e54c13` and new-server revision
`fa7930c8474fb650539afa26495a0a531898ab69`, with AIA, odd-sized FSI, and
homogeneous embedded and linked JPX fixtures. It checked every resolution,
limited continuation, multi-batch restoration, metadata, palettes, and KDU
pixel equivalence. This does not establish compatibility with every deployed
classical build, heterogeneous classical movies, or files outside the C
client's reconstruction profile. HTTP/channel handling still belongs to the
host and requires separate integration validation.

The V4 scratchpad review found a specific server-side limitation: the repository's
`tests/merge/fixtures/expected/merged.jpx` contains embedded frames of different
sizes. Classical `35aca93` and 1.9.0-rc1 `7ccac81` serve it correctly only at the
lowest resolution. Higher resolutions have missing precincts and one frame
causes a connection drop, with or without region padding. This occurs with any
client. The new server does not have that observed failure. The host must report
an unready normally completed window rather than display a padded reconstruction
as if it were complete. This fixture's old-server compatibility is therefore
limited to its lowest resolution; no general mixed-size compatibility is claimed.

A response refusal is terminal for the host's source, including cache-model
restoration refusal. Close its channel, discard persisted cache for that source,
and create a new client if retrying. Replay verification detects conflicts in
already-held bytes; it cannot detect changes confined to bytes not yet received.
The immutable-target requirement still applies.
