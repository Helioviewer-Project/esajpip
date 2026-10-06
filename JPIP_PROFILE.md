# JPIP support profile

esajpip implements the part of JPIP needed to serve JP2 and JPX imagery to
JHelioviewer. This document defines the wire behavior and the source-file
structures it supports, in the terminology of Annex J of
[ITU-T T.808 (12/2022) / ISO/IEC 15444-9:2023](https://www.itu.int/rec/T-REC-T.808-202212-I/en).

esajpip claims no Annex J profile. Its subset draws on several of them, and it
ignores unknown request fields for compatibility, so an accepted request does
not by itself prove that every field was honored.

The [README](README.md#concepts) defines the terms used throughout: codestream,
target, window, JPIP channel, data-bin, and JPP stream.

## Relationship to T.808 Annex J

Even Profile 0 requires complete semantics for fields such as `type`, `tid`,
and `pref`, for which esajpip implements only reduced or compatibility
behavior. Its subset instead draws from more than one Annex J level:

| Annex J area | esajpip behavior |
| --- | --- |
| Profile level | Some Profile 0 fields are supported, additive explicit cache-model descriptors from Profile 1 are supported in reduced form, and reduced `stream` and `context` selection comes from the Full Profile. |
| Return-data variant P | JPP-stream data is returned. JPT-stream and complete-file return types are not implemented. |
| Cache variants N and S | Additive explicit byte-count cache descriptors provide part of N behavior. A reduced S channel model lets `cnew`, `cid`, and `cclose` maintain a persistent cache for one target. Neither variant is complete: the full N grammar, session grouping, several channels in one session, and concurrent channel requests are missing. |
| Incremental codestream variant C | Main-header, tile-header, and precinct data-bins are delivered incrementally, but the `meta:incr` preference behavior required by the formal variant is not implemented. |
| Metadata variant M | JP2 and JPX box contents are delivered in metadata data-bins and `metareq` is recognized in reduced form, but the standard metadata-request grammar and `meta:orig` preference behavior are not implemented. |

The labels **Supported** and **Reduced** below describe this implementation,
not Annex J conformance.

## Wire profile

| Area | Support | Behavior and reason |
| --- | --- | --- |
| HTTP | Reduced | HTTP/1.1 `GET` requests in origin-form or absolute-form are accepted. For an absolute URI, the authority is discarded and its path selects the local target. Successful image responses use chunked transfer encoding; a successful `cclose` has an empty fixed-length body. Bounded llhttp parsing rejects other methods and HTTP versions. |
| Return type | JPP-stream only | Successful image responses use `image/jpp-stream`. JPT-stream, complete-file return types, and return-type negotiation are not implemented because JHelioviewer consumes precinct-based JPP-streams. |
| Transport | HTTP only | A `cnew` offer is accepted only when its transport list includes `http`, which is then selected in `JPIP-cnew`. A valid offer containing only unsupported transports receives `501` without a `JPIP-cnew` header because stateless service is not implemented. Auxiliary TCP, UDP, and upload transports are not implemented. |
| Sessions and channels | Stateful, reduced | `cnew`, `cid`, and `cclose` are supported. One channel owns one target and processes one request and response at a time. This matches JHelioviewer's access pattern and keeps cache and JPEG 2000 ownership explicit. |
| HTTP connection reuse | Supported | A later request for a `cid` may arrive on the same or a different connection, and one pooled connection may carry requests for different channels. `Connection: close` closes the connection after its response without ending the channel. Each channel has one active and one waiting request. The channel, not the HTTP connection, owns the JPIP session state. |
| HTTP request bodies | Not supported | Requests with a nonzero or invalid `Content-Length`, or with any `Transfer-Encoding`, receive `400 Bad Request`. The connection and any referenced channel are then closed so body bytes cannot be interpreted as another request. |
| Stateless requests | Not supported | Requests require `cnew`, `cid`, or a usable `cclose`. All cache and image state belongs to one channel. |
| Concurrent requests | Not supported | Responses are not preempted by a newer request and requests are not served concurrently within a channel. `qid`, `wait`, and window-change cancellation are not implemented. Serial service avoids shared JPEG 2000 state and is compatible with JHelioviewer. |
| Compression | Reduced | If a request contains `metareq` and `Accept-Encoding` contains `gzip`, the JPP response is gzip encoded. Other content codings and general HTTP content negotiation are not implemented. |
| Errors | Reduced | Identified requests receive `400` for malformed supported fields, `404` for a missing, invalidly named, unsupported, or profile-excluded target, `431` for a request line over 2 KiB or headers over 4 KiB, `501` for an unsupported channel transport, `503` for an invalid or conflicting channel state, or `500` for an unreadable source and other server failures. The server also returns `503` when the channel is unknown, has ended, or already has a request waiting. Each HTTP error response has a short plain-text body identifying the failure. An error terminates the connection and any referenced channel; routing rejections leave an existing channel unchanged. The rejected request does not modify the channel cache before termination. Traffic rejected before JPIP identification is closed without an HTTP response. The complete JPIP correction-header model is not implemented. |

The HTTP parser accepts at most a 2 KiB request line and passes the complete
request target to the JPIP parser. Request paths and `target` values are
percent-decoded once, rejecting malformed escapes and NUL; clients must escape
reserved characters in file names. The supported `model` and `context` grammars
also decode percent escapes. Initial routing and full request parsing use the
same query-field splitter. If a non-conforming request repeats a routing field,
both stages use its last value. Client-supplied URI paths and
`target` values containing a path segment equal to `..` are rejected. An
established channel accepts at most 2 KiB for the request line and a separate
4 KiB for all headers and their terminating blank line.

Client request examples, HTTP responses, connection replacement, timeout, and
recovery rules are documented in
[Connections and JPIP channels](CHANNELS.md).

## Request fields

| Field | Support | Current interpretation |
| --- | --- | --- |
| `target` | Supported on `cnew` | Selects the file. If absent, the request URI path selects it. A channel cannot change target after creation. Parent-directory path segments are rejected. |
| `cnew` | Reduced | Accepts a comma-separated transport list and creates one independent channel when `http` is offered. Channel association through a simultaneous `cid` is not implemented. A request combining `cnew` with `cclose` is rejected. |
| `cid` | Supported | Routes a request to an existing channel, including when it arrives on a replacement HTTP connection. Channel IDs contain 128 random bits encoded as 32 lowercase hexadecimal characters. They are opaque bearer capabilities and must not be guessed or derived from connection numbers. |
| `cclose` | Reduced | Closes one channel named by its exact channel ID. `cclose=*`, lists, and multi-channel session closure are not implemented. |
| `fsiz`, `roff`, `rsiz` | Reduced | Select a resolution and rectangular window of interest. `fsiz` must have positive dimensions and is required when either other window field is present. `roff` defaults to `(0,0)` and `rsiz` defaults to the area from that offset to the lower-right corner. `round-up`, `round-down`, and `closest` are recognized on `fsiz`; omitted rounding defaults to `round-down`, and `closest` compares image area. The requested window is mapped to the selected resolution by flooring its upper-left corner and ceiling its lower-right corner. As a compatibility extension, signed offsets are accepted and cropped to the image; request-region sizes must be non-negative. An empty intersection returns only `EOR WINDOW_DONE`. At every resolution up to the selected one, the served precincts are those whose partition cells intersect the cropped window, with cell indices computed as the floor of the window corners divided by the precinct size at that resolution, so a window edge on a partition boundary includes the cell it starts. Server-adjusted window response fields are not generated. |
| `stream` | Supported with a selection limit | Accepts the standard comma-separated list of single codestreams, inclusive `first-last` ranges, open `first-` ranges, and positive `:sampling-factor` qualifiers. Non-existent codestreams are ignored, overlapping ranges are combined, and omission selects codestream 0 unless `context` determines the selection. At most 100,001 existing codestreams may be selected across all `stream` and `context` fields in one request. Each selected codestream maps the requested window through its own dimensions and packet geometry, and their packets are delivered in interleaved order. |
| `context` | Reduced | Recognizes `jpxl` with one layer number, one ascending inclusive range, or one open-ended `first-` range, and treats the selected compositing-layer numbers as codestream numbers. Open ranges extend to the last available codestream; non-existent codestreams are ignored. The combined selection limit described for `stream` applies. Standard sampling factors, geometry suffixes, and other context syntax are rejected rather than ignored. JPX composition instructions and remapping are not implemented. This is sufficient for the one-codestream-per-frame JPX movies served to JHelioviewer. |
| `layers` | Supported | Requests the first specified quality layers of each selected codestream. Omission requests all layers; counts above a codestream's layer count also select all its layers. Zero requests metadata and headers without precinct packets. Increasing the count on a later request sends the uncached suffix. Precinct completion flags indicate the final source layer, not the requested limit. A completed layer-limited window returns EOR `window done` (2). |
| `len` | Supported | Limits the JPP message headers and data-bin payload generated for the response; the EOR message does not count toward the limit. The value is a ceiling, not a target: the writer reserves 60 bytes for framing and may leave up to 160 bytes unused rather than start a very small final fragment. When omitted, the server uses a response budget of 2,147,483,647 bytes. Exhausting this implicit budget returns EOR `response limit reached` (7); exhausting an explicit `len` budget returns EOR `byte limit reached` (4). In either case, the channel remains usable for continuation. Values below the three-byte EOR size are raised so that a valid EOR can still be sent. Negative and overflowing values are rejected. |
| `model` | Reduced | Accepts additive byte-prefix or complete-bin descriptors for metadata (`M`), main headers (`Hm`), tile headers (`H`), and precincts (`P`), with an optional single, closed, or open-ended codestream range. An unqualified descriptor applies to the first codestream in `stream`, or to codestream 0 when `stream` is absent, regardless of `context`. Each descriptor is validated against the selected image before it is applied. An invalid descriptor terminates the channel, so no partially applied model is reused. Since the served profile has one tile, only tile-header bin zero is valid. Implicit descriptors, subtractive descriptors, wildcards, tile descriptors, and layer-count descriptors are not supported. Because `mset` is ignored, it neither discards cache state for other codestreams nor restricts the codestreams affected by `model`. |
| `metareq` | Compatibility subset | Its presence is recognized, including JHelioviewer's `[*]!!` form, but the expression is not parsed. Metadata is sent according to the server's fixed JPX metadata-bin representation. The field also enables gzip when the HTTP client accepts it. |
| `tid` | Reduced | Does not affect target selection. A request carrying this field receives `JPIP-tid: 0` after the field has been recognized, including on early channel-routing errors. This indicates that the server does not assign a stable target identifier. If the supplied value is not `0`, the server disregards `model` because it belongs to a different target identity. |
| `handled` | Supported conservatively | Returns `JPIP-handled: tid,cid,cnew=http,stream,len,handled`. This deliberately advertises less than the complete accepted profile rather than overstating reduced or compatibility behavior. The header is also returned with errors after the field has been recognized, including early channel-routing errors. |
| `type` and unknown fields | Accepted but ignored | They do not affect serving. The server always returns JPP-stream, even when `type` excludes it, instead of rejecting the request as T.808 requires. This tolerance preserves existing JHelioviewer requests, but is one reason this is not an Annex J profile. |
| `qid`, `pref`, `mset`, `align`, `quality`, `drate` | Accepted but ignored; normative reduction | Their mandatory effects and responses are not implemented: no `qid` ordering or echo, no error or `JPIP-pref` for an unsupported `pref=/r`, no cache restriction or `JPIP-mset` for `mset`, no `JPIP-align: no` or `JPIP-quality: -1`, and no delivery-rate scheduling. |
| Other standard fields | Not supported | `subtarget`, `comps`, `srate`, `roi`, `wait`, `sendto`, `abandon`, `barrier`, `twait`, `tpmodel`, `need`, `tpneed`, `cap`, `csf`, and upload fields have no implemented semantics. Some are correctly inert in this profile: `sendto`, `abandon`, and `barrier` apply to the unsupported HTTP-UDP transport; an unknown `roi` may be ignored; and serial channel processing already completes each response before reading the next, as `wait=yes` requests. The remaining fields are accepted without applying their standard selection, timing, cache, capability, or response behavior. |

The server does not reject every unsupported field. Clients should rely on the
table above rather than treating a successful response as proof that every
field was understood.

## Responses and cache model

A completed successful image response contains a sequence of JPP messages
followed by exactly one end-of-response message. As needed for the request and
the channel's cache model, the server emits:

- Metadata data-bins for the JP2 or JPX box structure.
- Codestream main-header data-bins.
- An empty, complete tile-header data-bin for tile 0. Tile-part header marker
  segments from the source are not delivered to the client.
- Precinct data-bins containing packets relevant to the requested window.
- `window done`, `byte limit reached`, or `response limit reached`
  end-of-response reasons.

Data-bin offsets and completion flags let the client assemble data across
responses. The channel remembers how much of each metadata, header, and precinct
bin it has sent. An additive `model` request can extend that record. This state
belongs to one channel. It is not shared with another client and cannot be
recovered after the channel or server process is lost.

On channel creation, the response includes `JPIP-cnew` with the assigned `cid`,
`path=jpip` and `transport=http`. It also includes `JPIP-tid: 0`. Later
successful requests repeat `JPIP-tid: 0` when the request carries `tid`.
The value explicitly indicates that the server does not assign stable target
identifiers or guarantee target identity across sessions. The remaining JPIP
response preference and correction headers are not emitted. This includes
`JPIP-len`: when a nonzero `len` is too small to make progress, the response
contains only EOR and gives no suggested minimum for the next request
(T.808 D.2.18).

The `path=jpip` value is deliberately an ABNF-compatible token rather than an
echo of the request URI path. It directs subsequent requests to `/jpip`;
JHelioviewer constructs that path by prepending the slash. Returning
`path=/jpip` would conflict with the formal response grammar.

## Supported JPEG 2000 sources

The server must find packet boundaries without decoding or transcoding the
image. For that reason, it accepts a narrower set of files than the full JP2 and
JPX specifications allow.

The lists below describe the source layout that the serving code expects, not
a complete JP2 or JPX conformance check. esajpip validates the fields it needs
for packet indexing, data-bin construction, links, and file bounds. It reads but
does not enforce the file-type minor version, does not use codestream `Rsiz` to
reject unsupported extensions, and does not check every mandatory box or box
ordering rule. Conversely, the JPX parser requires a `jpch` box for every
codestream even where the JPX specification permits defaults to be inherited.
A successful open therefore means that a file matches this parser's operational
expectations, not that it conforms completely to JP2 or JPX.

### JP2

- The file name must end in `.jp2`.
- The first box must be the standard JPEG 2000 signature box, followed by the
  file-type box with the `jp2 ` brand and compatibility entry.
- Exactly one embedded codestream is supported.
- The codestream must contain one tile and use zero image and tile origins.
- All five Part 1 progression orders are indexed. Components must use unit
  sampling because the packet index has one shared precinct geometry.
- Explicit precinct dimensions and the Part 1 default of 32,768 by 32,768
  samples are supported. An explicit zero exponent is accepted only at the
  lowest resolution.
- The main header must contain exactly one `SIZ`, one `COD`, and one `QCD`
  marker and must contain all information needed to decode every tile-part.
  Main-header QCD and COM bodies are forwarded unchanged without decoding
  their quantization or comment fields. Their segment boundaries are checked.
  `COD`, `COC`, `QCD`, and other non-`PLT` marker segments in tile-part headers,
  `PPT` included, are rejected because tile-part headers are not delivered to
  the client. Main-header `COC` and `POC` markers are rejected because packet
  indexing is derived from the main `COD` marker. A main-header `PPM` marker is
  rejected because it moves the packet headers into the main header: precinct
  data-bins, built from the tile-part data, would hold packet bodies without
  their headers. `PPT`, `SOP` and `EPH`, which T.800 places in tile-part
  headers and packets, `0xFF00`, which is not a marker, and `0xFF30` to
  `0xFF3F`, which have no marker segment to skip, are rejected in the main
  header. Main-header component quantization and region markers are
  preserved but do not affect packet indexing; other length-delimited
  markers are skipped.
- Code-block style bits defined by Part 1 are accepted. Reserved bits, including
  the HTJ2K flag, are not supported.
- SOP marker segments are outside the served profile because they precede the
  packet represented by each precinct data-bin. EPH markers remain supported.
- The Part 1 multiple component transform is accepted when the codestream has
  at least three components and the first three have equal bit depth, as
  required by T.800.
- Up to 64 tile-parts and multiple `PLT` markers may be indexed, subject to the
  validated marker, packet, and tile-part bounds. `TPsot` must start at zero
  and increase by one. If `TNsot` is nonzero, it must equal the final
  tile-part count. After tile-part data, only the next `SOT` or the final `EOC`
  marker may follow. A tile-part with `Psot = 0` runs up to the `EOC` that
  ends the codestream, so it is the last tile-part; its packet data is not
  scanned for markers. Each tile-part's `PLT` lengths must exactly cover its packet
  data, and no packet data may follow the packet set derived from `COD`. Marker
  structure is checked while opening the source; coverage and packet bounds are
  checked lazily as packets are indexed. As a compatibility exception for
  deployed JPEG 2000 files, zero `Iplt` entries after the logical packet list
  are ignored; a nonzero trailing entry is rejected. An `Iplt` entry has at
  most ten bytes, enough for any 64-bit length.
- Packet counts, packet locations, and each data-bin's cumulative byte length
  must fit the signed 32-bit JPIP state. Source files larger than `INT_MAX`
  bytes are outside the supported profile.

Files without `PLT` packet-length information are rejected. esajpip does not
decode packets to rediscover their boundaries and does not replace the separate
transcoding step required for such inputs (`hv_transcode`,
[`transcode/`](transcode/README.md)).

### JPX

- The file name must end in `.jpx`.
- The first box must be the standard JPEG 2000 signature box, followed by the
  file-type box with the `jpx ` brand and compatibility entry.
- A JPX with no links must contain every declared codestream. Each embedded
  codestream keeps its own coding parameters.
- If links are present, every declared codestream must have a link in the
  supported fragment-table/data-reference form.
- Codestreams are numbered by the physical order of their contiguous
  codestream or fragment table boxes, independently of codestream header order.
- A linked codestream uses one `flst` entry containing exactly one fragment.
  The fragment must describe the complete codestream indexed in the referenced
  file.
- Codestream header, contiguous codestream, and fragment table boxes must be at
  the JPX file's top level. Multiple Codestream (`j2cx`) boxes and other nested
  codestream container forms are not supported.
- Data references use version-zero `file://` URL boxes. Remote HTTP URLs,
  multiple-fragment codestreams, and serving a mixture of embedded and linked
  codestreams are not supported. Relative paths are resolved from the directory
  containing the JPX file.
- Each link must resolve to a lowercase `.jp2` file with one embedded
  codestream. JPX-to-JPX links are not supported.
- Top-level association boxes are exposed as separate metadata bins. Other box
  contents are preserved in metadata, but esajpip does not interpret the full
  JPX composition model.

This linked profile matches the movies produced for JHelioviewer by
`hv_merge` ([`merge/`](merge/README.md)) and hvJP2K's `hv_jpx_merge`, and
the compatible `kdu_merge` form used in deployment. It
preserves codestream order and validates link counts, reference indices,
fragment ranges, and source codestream structure before serving.

Linked JPX files are trusted server-side inputs. Their decoded `file://`
references are opened as local paths. JPIP protocol handling neither fetches
them over the network nor confines them to the configured image directory.

Raw codestream files and other JPEG 2000 family extensions are not accepted.

## Why this profile is narrow

The main production workload is a linked JPX movie with one codestream per
frame. JHelioviewer first requests metadata and then asks for rectangular
precinct windows. Serving that layout directly avoids decoding or rewriting the
source data and keeps each client's memory inside its own channel.

Each channel exclusively owns its file index, linked-JPX graph, cache model,
and traversal state. One thread operates on that state at a time; routing
stays on the control thread. Broader standard support should preserve this
ownership rule. New syntax or file-model behavior belongs inside a channel and
must not introduce shared JPEG 2000 state between channels.

Add unsupported features when a real client or source file needs them and their
data-bin behavior can be checked against T.808. Ignoring an unknown field for
compatibility is not a conformance claim.
