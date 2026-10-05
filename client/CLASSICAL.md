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
`cid=<cid>&stream=<frame>&len=<budget>`, without window fields.
For a prepared frame, use W,H from that frame's `hvc_view`, at its
requested resolution. Do not reuse another frame's geometry or approximate
fit dimensions. Request:

```text
cid=<cid>&stream=<frame>&fsiz=W,H,closest&rsiz=W+1,H+1&roff=0,0&len=<budget>
```

Evaluate W+1 and H+1 numerically. The one-pixel padding compensates for the
classical precinct-selection calculation, which can omit the last precinct
when the last image coordinate is exactly a precinct boundary. The new
server clips this window to the image. With exact frame dimensions, padding
does not change the resolution chosen by `fsiz` or the decoding geometry.
With approximate dimensions, the older server can scale the padded region
past the image and fail during packet lookup. Do not pad a priming request
before the frame's own geometry is known.

Response continuation is general client behavior, not an older-server
workaround. After every response, call `hvc_prepare` again. A byte-limit
EOR retains the bytes but does not confirm window completion. Repeat the prepared request
on the same channel until READY, then reconstruct. Do not change the window
while accumulating an unfinished request. The JHV frame budget is 2 MiB.

## Replacing a channel

Keep the client for the same immutable target. The classical server needs a
fresh persistent connection and channel. Do not use the new-server recipe
`stream=<frame count>` to select no codestreams on a classical server.

After successful initialization, frame 0's main and empty tile headers are
cached. Start a `hvc_model` cursor at zero and send each returned batch
with:

```text
stream=0&model=[0]Hm,[0]H0,<batch>&len=2000000
```

Add `cnew=http&type=jpp-stream&tid=0` and the original target for the first
batch, then the returned `cid` for later batches. Omit window fields. The
explicit complete header declarations suppress unsolicited header replay
before their descriptors appear in later model batches. Keep `len` after
`model`: the classical parser can lose a terminal descriptor at query EOF.
The model's partial precinct amounts are additive, so send each batch once.

Pass each restoration response to `hvc_restore_response`; it validates
any repeated metadata against the retained bytes without modifying the cache
or completing a pending frame request. Stop at an empty model batch, then
resume the interrupted window. This procedure requires complete initial
metadata and cached frame-0 headers, not just a successful channel opening.

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
