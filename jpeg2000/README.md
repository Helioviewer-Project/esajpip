# JPEG 2000 library

The JPEG 2000 reader/writer, built by the repository's CMake as the static
library `jpeg2000`. `hv_transcode` (in `../transcode/`) and `hv_merge`
(in `../merge/`) use it. The server uses it through `jpip`. The header types come from
the ASN.1/ACN model in `../spec/`; the code here steps from header to
header and records where each payload starts and ends.

Nothing in the reader, rules, writer, geometry, `hv_rewrite` or
`hv_served` keeps mutable static or global state (the generated code's
static bit patterns are only read), so server threads can use them at the
same time on different files. Command-specific output replacement and signal
handling live in `../tools/hv_file`, compiled directly into the two commands
and its test, outside `jpeg2000`.

## Public API

Use the smallest operation that supplies the information being consumed. The
same bounded readers and rules serve all callers; callers do not assemble flags
or validate opaque bodies just to copy them.

| Consumer | Read operation | Output requirement |
| --- | --- | --- |
| Merge | Bounded JP2 and selected image headers | `HV_OUTPUT_JPEG2000` copies codestreams; `HV_OUTPUT_JPIP` also verifies serving compatibility |
| Raw or file transcode | `HV_READ_PACKETS` or `HV_READ_PACKETS_JPIP`, selected internally from the output profile | The same two `hv_output_profile` choices as merge |
| Server | `hv_served_jp2/jpx`, then `HV_READ_JPIP_INDEX` | PLT validation completes when packets are consumed |
| JPIP client | `HV_READ_JPIP` for the received header; shared metadata readers | Supported precinct reconstruction |
| Local client | `hv_local_open/read`, then requested presentation/metadata queries | Original codestream bytes; the host decoder admits decoding capabilities |
| Validator / walk / rewrite | `HV_READ_VALIDATE`, `HV_READ_PADDED`, or `HV_READ_JPIP` | The selected validation contract; rewrite requires decoded bodies |

`hv_codestream_open` takes one `hv_read_mode`, not a bitmask. There are six
operations: validation, validation accepting PLT padding, JPIP validation, JPIP
indexing, packet reading, and packet reading with JPIP geometry. Unused flag
combinations and the separate packet-open API have been removed. `hv_plt_init`
takes its own PLT policy directly. Index mode must not be reported as complete
validation before its deferred PLT checks finish.

`hv_reader.h` provides bounded container and codestream operations.
`hv_presentation.h` provides layer, registration, color and channel access.
`hv_served.h` contains serving-only container admission and whole-file checks.
`hv_profile.h` defines the output choices shared by merge and transcode.
`hv_local.h` owns mappings and fragment access; `hv_metadata.h` indexes XML and
palettes; `hv_render.h` resolves supported display channels. Reading, presentation
and explicit validation use the same underlying bytes without hidden copies.

Container access leaves payloads opaque. `hv_read_siz` reads only bounded grid
and component fields. `hv_read_jp2h_boxes` reads selected image headers and
checks their consistency without scanning discarded file boxes. Whole-file
header checks and serving checks are separate, explicitly named operations.

Opening an owning local source does not select a rendering profile. Presentation,
metadata and SIZ queries are independent of byte access. For example, malformed
SIZ prevents a SIZ query but does not prevent copying its bounded codestream;
malformed COD or packet data does not affect a valid SIZ-only query. The decoder
or explicit validator remains responsible for accepting those bytes.

## Layout

| File | Role |
| --- | --- |
| `hv_reader.h` / `.c` | Steps through a file in memory: boxes (T.800 I.4) and codestream items (Annex A). Checks framing and decodes marker segments with the generated code, which also gives the bytes a variable-size field takes (`HV_DECODE_USED`, `hv_decode.h`), one element at a time (see below). Never copies a payload. A codestream item carries the decoded segment: SIZ as `hv_siz` (`SizFixed` and its `Component`s), COD and QCD as `CodSegment` and `QcdSegment`, PLT as `hv_plt` (Zplt and where its entries are, read with `hv_plt_next`), COM as `hv_com` (Rcom and its text, in place). |
| `hv_codes.h` | The marker codes, box types and brands the code refers to, the box header sizes, `HV_LARGEST(T)`, the size of a generated type's largest encoding (its `_REQUIRED_BYTES_FOR_ACN_ENCODING`), and `HV_FIXED(T)`, the size of a fixed-size type as the standards lay it out, listed for each type it is used with and checked against the model's (`hv_reader.c`): used instead of hand-counted layout sizes. |
| `hv_decode.h` | The generated decoders on a bounded window, for the reader, the rules, `hv_rewrite` and `hv_merge`: exactly the bytes of the enclosing box or segment, and the bytes the decoder read (`HV_DECODE_USED`). |
| `hv_rules.h` / `.c` | The model's cross-field rules, for both layers: on marker segment bodies (SIZ, COD, the tile-parts, the PLT entries and the packet count), the segments of a codestream across its headers (`hv_segments`: the bodies the framing keeps opaque, QCD, the coding and quantization of each tile-component, Profile 0 and 1, TLM, PLM, PPM and PPT against the tile-parts, SOP and EPH in the data; standard layer), the box tree (`hv_rule_box_tree`, on the file's bytes; `hv_rule_box_placed`, the same rules on one box copied into a JPX file) and the fragments in the file, the JPX boxes, and the JP2 header boxes (standard layer), and the profile's limit of 64 tile-parts. Also the box iterator (`hv_boxes_*`) and the File Type box's reader (`hv_ftyp_*`), which tell a JP2 file from a JPX file by the brand. The corpus harness (`../spec/harness/crossfield_impl.h`, `crossfield.c`) uses the same code, so the reader applies the rules the corpus labels come from. A check returns a rule name or NULL and does not allocate; `hv_rule_tiles` and `hv_rule_packets` are the two counts, not checks. |
| `hv_grid.h` | Shared nonnegative resolution rounding and precinct-cell counts, used by the rules, general geometry and compact JPIP geometry. |
| `hv_geometry.h` / `.c` | The resolutions, bands, precincts and code-blocks of one tile, and its packets in progression order (T.800 B.2 to B.7, B.12), from the decoded SIZ and COD. Counts and derives the partition instead of storing it; numbers code-blocks so that two precinct partitions with the same code-block partition agree. No COC or POC; at most 2,147,483,647 packets. Checks decoded SIZ/COD field ranges and layout requirements without classifying Rsiz declarations, and lays out no more resolutions and packets than the caller's `hv_geometry_limits` allow, since a header can declare far more than its data holds. |
| `hv_writer.h` / `.c` | Writes box headers, SIZ, COD, QCD, COM, PLT, SOT and opaque segments, the File Type box and Component Mapping entries, the JPX boxes the server reads (`flst`, `url`, and a `dtbl`'s NDR), the Reader Requirements box and the Number List box (`hv_write_nlst`), with the generated encoders into a growing buffer, one element at a time. Measures every length it fills in: Lxxx when a SIZ, PLT, COM or opaque segment ends, Psot when a tile-part ends, LBox/XLBox when a box ends (switching a box to XLBox if it outgrows LBox); `hv_box_header_size` gives a box header's size for a caller that writes a superbox's length before its contents. Splits PLT the way Kakadu does (as many whole entries as fit in Lplt = 65,535), once per tile-part, or writes one PLT segment as given (`hv_write_plt_segment`). Each encoder runs with room for its type's largest encoding, at most 197 bytes (`QcdSegment`), because the generated encoders do not check the room. The buffer's spare capacity is kept zero, because the generated encoders skip zero bits (`BitStream_AppendNBitZero` only moves on): an append clears nothing, and `hv_out_rewind` zeroes what it drops. |
| `hv_error.h` / `.c` | `hv_fail`: formats an error message into the caller's buffer and returns -1, for `hv_geometry` and the tools; `hv_grouped`: a number with thousands separators, as messages state limits. |
| `hv_served.h` / `.c` | The served profile's checks of a whole file on disk (`hv_check_served`), the files a JPX file links to included, optionally confined to a root directory (`hv_path_within`: symbolic links and `..` resolved), and the file reader they use, which reads regular files only, never blocks on a FIFO and refuses a file over a size limit before reading it (a linked file over INT_MAX bytes is `file.size-limit`): shared by `hv_walk -P` and `test_profile`. It reads each file whole; a server maps its files and uses the reader's functions on the mapping. |
| `hv_mapping.c` | The one ACN mapping function the generated code calls (`lxxx`: Lxxx counts itself), shared with the corpus harness. |
| `hv_rewrite.h` / `.c` | Writes a file again with the writer from what the reader decoded: every box and codestream item, the JPX boxes the writer has functions for (`flst`, `url`, a `dtbl`'s NDR, `rreq`) with them, PLT split as in the input. A file in the writer's form comes out byte for byte; the result says why another one differs (LBox = 0 or Psot = 0 written as lengths, zero PLT entries dropped, PLT entries in more bytes than needed). PLT segments out of the order of their Zplt, which T.800 allows and the writer does not write, are refused. Failures rewind the output to its original size; deferred PLT is refused. For checking the writer: `hv_walk -w`, `../tests/jpeg2000/test_rewrite.c` and `../spec/harness/writers.c`. |
| `hv_walk.c` | `hv_walk [-v] [-p] [-P [-R dir]] [-H] [-w] file...`: checks files with the reader and prints one line per file: `valid`, `invalid` with the error and its offset, `differs` when `-w` rewrote a valid file differently, `ERROR` when it cannot be read. `-v` lists every box and codestream item, `-p` accepts trailing zero PLT entries (`-p` with `-P` is a usage error), `-P` checks the served profile (`hv_check_served`: a `.jpx` with its linked files, whose codestreams it counts as linked; with `-R dir`, linked files must lie inside `dir`), `-H` the box structure and the header boxes at the standard layer (`hv_check_headers`: JPX for the `ftyp` brand `jpx `, JP2 for any other), `-H` alone leaves codestream bodies opaque and reports `headers only`; adding `-P` or `-w` also validates codestreams. `-w` rewrites the whole file with the writer from what the reader decoded (`hv_rewrite`) and compares it with the input. For `-P` a file is JPX when its name ends in `.jpx` in any case (the server picks the format by the extension), and JP2 otherwise; without `-P` and `-H` a file that starts with SOC is read as a raw codestream, while `-P` and `-H` fail it with `file.signature`. |
| `../tests/jpeg2000/` | The tests (see "Tests" below). |
| `generated/` | Code generated from `../spec/` by `generate.sh`: the types its `pdus` list names (those of `../spec/jpeg2000-io.asn1`, the bodies `hv_rules.c` reads one element at a time, the served-profile types the reader checks against, and the JPX, box tree, File Type and JP2 header box types) and what they use. A type the code here needs that none of them uses must be added to `pdus`. |
| `generate.sh` | Regenerates `generated/` with the pinned asn1scc (Docker image, or `ASN1SCC=...`); `--check` compares without replacing it (`../spec/check-model.sh` runs it so). |
| `CMakeLists.txt` | The `jpeg2000` library and the `hv_walk` tool, added by the top-level `CMakeLists.txt`; the tests under `tests/`. |

## Build

With the rest of the repository, so configuring needs the server's
dependencies too: the top-level `CMakeLists.txt` also adds `server/`, which
requires zlib, glib and llhttp.

```sh
cmake -S . -B build [-DESAJPIP_SANITIZE=ON]
cmake --build build --target hv_walk
```

## Tests

The shared runner builds and runs the library, tool and server tests:
`tests/run.sh [normal|sanitize] [CTest options]`. With no arguments it runs
normally. Put CTest options after an explicit mode, for example
`tests/run.sh normal -L tools`. Builds go to `build/tests-<mode>`, or to
`ESAJPIP_TEST_BUILD_DIR`.

- `test_profile`: every vector of `../tests/vectors/j2k` against its
  manifest labels: the served profile (`hv_check_served`) and the header
  box checks (`hv_check_jp2h`, `hv_check_jpx_headers`), which must name
  the rule the manifest gives; and unit checks the corpus cannot reach.
- `test_rewrite`: every vector the reader accepts, written again with
  `hv_rewrite`, comes back byte for byte, or in the writer's form and
  passing the same checks.
- `test_reader`: `hv_reader.c` included, for what the corpus does not
  reach: each rejecting branch with its message and offset, the items of
  `hv_codestream_next`, every proper prefix of the base files, and out of
  memory at each allocation.
- `test_writer`: `hv_writer.c` included with lowered limits: the XLBox
  switch, the Psot limit, measured lengths, the PLT split, writes after an
  error and out of memory.
- `test_geometry`: `hv_geometry.c` included, against T.800 computed a
  second way on random SIZ and COD segments (the grids from the equations,
  the packet order from the loops of B.12.1), and its limits.
- `test_served`: `hv_served.c` included, with system calls that fail on
  demand: loading files, the root check, and linked files.
- `test_file`: `hv_file`: commit, abort, mode, owner and group, symbolic
  links, a second `hv_file` while one is open, and a signal while writing.

After a model change in `../spec/`, run `jpeg2000/generate.sh`. It needs the
compiler that `../spec/build-asn1scc.sh` builds: the unmodified upstream
revision pinned in `../spec/VERSION`. Former local fixes are archived in
`../spec/asn1scc-patches/reference/`. The upstream fixes for
`0004-icdpdus-reference-init` retain the initializers required by `-icdPdus`;
`0001-deferred-determinant-uninit` prevents the PLT decoder from reading an
uninitialized flag on truncated input.

## Hostile input

The work of every check is in proportion to its input, whatever a header
declares: a SIZ of 65,535 tiles costs what its tile-parts cost, since the
reader counts tile-parts per tile in pages of 256 tiles that it allocates
when one of their tiles appears, and a codestream's unfinished tiles as it
goes; a `cdef` box of up to 64 entries is checked pairwise, a larger one
by sorting (`HV_CDEF_PAIRWISE`); and `hv_check_served` reads a linked
file once, however many links name it (by device, inode and size, with
the same fragment). A server that checks links on its mappings needs the
same care. What stays with the caller: `hv_geometry` lays out what its
limits allow (12 bytes a packet, 360 a component-resolution), and the
tools bound those by the input.

## One element at a time

No code here holds a list as a generated struct at the standard's bound.
The types the reader and writer use are small: a fixed part or one
element of a list (of `../spec/jpeg2000-io.asn1` or of the whole-file
model), or a whole COD or QCD segment (`CodSegment`, `QcdSegment`), which
are small enough (at most 45 and 197 bytes) to decode in one piece. The
reader decodes a list element by element up to the end its segment or box
length gives:

- SIZ: `SizFixed`, then `Component`s to the end of the segment, as many as
  a Csiz can count (else `invalid SIZ`), into an array allocated for them;
  `siz.csiz-count` compares Csiz with the number decoded. In the profile,
  after the named rules, `SizFixed-Profile`, the part of `Siz-Profile` they
  do not cover (`Component-Profile` is `siz.component-sampling`).
- PLT: `Zplt`, then entries to the end of the segment, decoded once.
  Diagnostic precedence is structural decoding (`invalid PLT`), then the
  Zplt of the header (in the profile
  `plt.zplt-sequence`, 0, 1, 2, ... in segment order; at the standard layer
  each once, in any order, `plt.zplt-index`), then each entry
  (`plt.value-overflow`, the padding rules), then the header's entries
  against the data at SOD (`plt.coverage`, `plt.zero-length`). An entry is
  an `Iplt`, at most ten bytes, in the profile; at the standard layer any
  number of leading zero groups (Table A.36 sets no limit). The entries
  stay in the caller's buffer; `hv_plt_next` reads them as the standard
  layer does.
- COC, QCC, RGN, POC, TLM, PLM, PPM, PPT and CRG, at the standard layer:
  their bodies one element at a time (`CocStyle`, `ComponentIndex`, `Qcd`,
  `RgnStyle`, `PocChange`, `TlmHeader`, `Ttlm`, `Ptlm`, `Zplm`, `Zppm`,
  `Zppt`, `CrgEntry`; `invalid XXX` where they are not those), in place
  until EOC, and the rules of `hv_segments`.
- COM: `Rcom`, then at least one byte of text, which stays in place.
- `rreq`: `RreqHeader` (ML, FUAM, DCM, NSF), NSF `RreqStandardFeature`s,
  NVF (`FeatureCount`), NVF `RreqVendorFeature`s, each feature handed
  exactly its code and ML bytes, and `rreq.extent` for bytes after them.
- `flst`: NF (`FragmentCount`), then the `Fragment`s, one at a time.

The writer encodes the same elements one at a time and never takes a
length from a type's size: it measures what it wrote (a segment's Lxxx,
Psot, LBox). `HV_FIXED(T)` appears only for fixed-size types (a marker
code, Lxxx, SOT, `Component`, `Fragment`, a feature's code); wherever a
measured length works, the reader uses `HV_DECODE_USED` instead.

`hv_siz` is SIZ as the rules and the geometry take it, however it was
decoded: the reader points it at its `SizFixed` and component array, the
corpus harness at the whole-file model's `Siz`
(`{&siz.fixed, siz.components.arr, nCount}`).

## Struct sizes

The asn1scc revision in `../spec/VERSION`, C, 64-bit: the C struct and the
largest encoding of each type the reader and writer decode or encode.

| Type | struct (bytes) | largest encoding (bytes) |
| --- | ---: | ---: |
| `BoxHeader` | 32 | 16 |
| `FtypHeader` | 16 | 8 |
| `SotSegment` | 40 | 10 |
| `SizFixed` | 80 | 36 |
| `Component` | 32 | 3 |
| `CodSegment` | 616 | 45 |
| `QcdSegment` | 208 | 197 |
| `Zplt` | 8 | 1 |
| `Iplt` | 88 | 10 |
| `Rcom` | 8 | 2 |
| `UrlHeader` | 16 | 4 |
| `RreqHeader` | 32 | 19 |
| `RreqStandardFeature` | 24 | 10 |
| `RreqVendorFeature` | 28 | 24 |
| `Fragment` | 24 | 14 |
| `Ihdr` | 128 | 78 |
| `ColrHeader` | 40 | 7 |
| `PclrCounts` | 16 | 3 |
| `CmapEntry` | 24 | 4 |
| `CdefEntry` | 24 | 6 |
| `NlstEntry` | 16 | 4 |
| `Resolution` | 120 | 74 |
| `UuidId` | 16 | 16 |

`MarkerCode`, `SegmentLength`, `DataReferenceCount`, `FeatureCount`,
`FragmentCount`, `BitDepth`, `CdefCount`, `UuidCount` and `CrefType` are
8-byte integers. `hv_codestream` is 10,616 bytes, of which `hv_segments`,
the state of the rules across segments, is 8,568 (the PLM and PPM series
and TLM's entries, 256 segments each); it allocates only SIZ's components
(32 bytes each, at most 16,384: more is `invalid SIZ`), at the standard
layer each component's state for `hv_segments` (60 bytes each), and the
per-tile tile-part counts. `hv_header`, for the header box checks, is 832
bytes: of the `bpcc`, `cmap` and `cdef` boxes it keeps where the entries
are, which the file's buffer holds until the codestream's header is
checked, and the bit depths of the palette's columns. A `cdef` box's
entries are allocated while it is read, with room to sort them (32 bytes
each, at most 65,535), and the header checks allocate the extents of the
file's `mdat` boxes (16 bytes each).

## What the reader checks

The standard validator covers Rsiz 0, 1 and 2 and the existing Part 2
checks. Recognized cinema, broadcast, IMF and PRF-based declarations return
`siz.unsupported-profile`; unhandled CAP signalling returns
`siz.unsupported-capabilities`. These are validation limits, not claims
that T.800 reserves those declarations. CAP and PRF main-header segments
are preserved as opaque segments; their syntax and ordering are not fully
validated. The serving profile continues to preserve Rsiz and checks its
own packet-layout requirements independently.

Geometry uses `hv_rule_siz_geometry` for component count, image/tile extents,
tile count and MCT geometry. The full `hv_rule_siz` validator shares those
checks while retaining its standard or serving declaration policy. A layout
calculation does not establish conformance to the profile named by Rsiz.
The packet reader uses the same layout checks without declaration validation;
`HV_READ_PACKETS_JPIP` additionally applies serving header restrictions. Writers
that retain a declaration must apply their own output policy. `hv_rule_rsiz`
shares the standard validator's declaration support classification with such
writers; it does not validate the declared profile's requirements.


- Boxes: LBox, and XLBox when LBox = 1, within the container; LBox = 0 only
  for the last box, and inside a superbox only if the superbox also runs to
  the end of the file.
- Codestream: SOC then SIZ; exactly one COD and one QCD in the main header;
  marker placement per Table A.2; tile-part order, TPsot and TNsot; Psot, or
  up to the final EOC when Psot = 0; nothing after EOC. A tile-part header
  that reaches SOT or EOC is "tile-part header without SOD" in every operation.
  The accessors give the main header's SIZ, COD and (outside `HV_READ_JPIP`)
  QCD bodies once read,
  and NULL before, for a segment the reader rejected, and after
  `hv_codestream_close`.
- Under `HV_READ_JPIP`, QCD and COM bodies remain opaque. Their segment
  boundaries and placement are checked, and QCD must occur exactly once
  in the main header. The QCD accessor and QCD/COM item pointers are NULL.
- Bodies, by the generated decoders outside that exception: COD and QCD
  whole, SIZ, PLT and COM
  one element at a time, and at the standard layer the bodies above, plus
  the model's cross-field rules (`hv_rules.c`): SIZ, COD, zero packet
  lengths, PLT sums against the tile-part data, and at the standard layer
  the segments across headers (`hv_segments`). Where the layout gives the
  packets (every tile-part has PLT, one tile, zero origins, unit sampling,
  no COC, POC, PPM or PPT, no tile-part COD, and not
  `HV_READ_PADDED`), also one PLT entry per packet
  (`plt.packet-count`), as the model's standard layer.
- `HV_READ_PADDED` (`hv_walk -p`) accepts trailing zero PLT entries in each
  tile-part. Nonzero lengths after padding remain an error. This is a separate
  validation operation; it is not combined with the serving mode.
- `HV_READ_JPIP` (`hv_walk -P`) enforces serving geometry, supported markers,
  tile-part limits and complete PLT coverage. QCD/COM bodies remain opaque.
  `HV_READ_JPIP_INDEX` uses the same structure requirements but validates PLT
  entries when the index consumes them.
- `HV_READ_PACKETS` and `HV_READ_PACKETS_JPIP` interpret SIZ/COD and tile framing
  for packet processing. The latter requires serving geometry. Neither reads
  input PLT lengths or validates copied/dropped marker bodies; the consumer
  checks the packet headers it uses and rejects unsupported layout markers.
- `hv_served_jp2` (`hv_walk -P`), the profile's file rules for a JP2 file:
  at most `INT_MAX` bytes, the signature box, then the file type box with
  the `jp2 ` brand and compatibility entry, top-level boxes framed as above,
  exactly one `jp2c`. A raw codestream fails.
- `hv_served_jpx` (`hv_walk -P` on a `.jpx`), the profile's file rules for
  a JPX file, as served through `ImageIndex`: the signature and a file type box with
  the `jpx ` brand; the children of `jpch`, `ftbl` and `dtbl`, placed as
  T.801 Annex M requires (`hv_rule_child`); the count rules
  (`hv_rule_jpx`); one fragment per `ftbl` (a fragment with OFF below 12
  fails as such, not as `flst.one-fragment`), and version-0 `file://` URLs
  naming `.jp2` files, whose percent-escapes decode
  (`url.percent-encoding`). It returns the codestreams, embedded or linked;
  `hv_codestream_check` reads
  an embedded one, `hv_served_path` resolves a link as the server does (or
  says why it cannot), and `hv_served_link` checks the linked file and that
  the fragment is exactly its codestream. `flst.dr-range` and
  `flst.dr-external` point at the `flst` box. Other boxes, `asoc`
  included, are opaque, as they are to the server. `hv_check_served`
  (`hv_walk -P`) does all of it for a file on disk.
- `hv_check_jp2h` and `hv_check_jpx_headers` (`hv_walk -H`, which picks
  one by the `ftyp` brand, as `hv_check_headers` does: `jpx ` is a JPX
  file, any other brand a JP2 file), the standard layer of the box
  structure and the header boxes (the server reads none of them, but a
  client decodes the image by them): first the start of the file, as the
  profile's checks (signature, `ftyp` second, `jp2 ` in its list for a JP2
  file, the brand and `jpx ` in its list for a JPX file, two boxes at
  least); for a JPX file the count rules of `hv_rule_jpx` at the standard
  layer (the Reader Requirements box, one and the third; at most one
  `dtbl`; a codestream per `jpch` unless a `j2cx` or `jclx` holds more);
  then the box tree (`hv_rule_box_tree`: the placement and count rules of
  T.800 Annex I, and in a JPX file of T.801 Annex M, in every superbox the
  file's kind defines, up to 32 nested superboxes; a deeper superbox
  returns `box.depth-limit`, meaning incomplete validation rather than a
  standards violation; and the contents of `uinf`, `ulst`, `uuid`, a
  `uinf`'s or `dtbl`'s `url`, `lbl` and a `cref`'s type); in a JPX file
  the fragment lists (`hv_rule_fragments`: exactly NF complete, decodable
  tuples, DR within the validated Data Reference table, local fragments in
  an `mdat`, the first at SOC, including lists inside `j2cx`); MinV (`file.ftyp-minor`: 0 for JP2, 1 for JPX); in a JPX
  file the Reader Requirements box's contents, decoded one part at a time
  (above), without deprecated features; where `jp2h` is (for JPX at most
  one, and in a baseline file, `jpxb`, before the first `jp2c`, `ftbl`,
  `mdat`, `jpch` and `jplh`); the
  children of `jp2h`, `jpch` and `jplh`, decoded one box or entry at a
  time with the model's types (`Ihdr`, `BitDepth`, `ColrHeader`,
  `PclrCounts`, `CmapEntry`, `CdefCount`, `CdefEntry`, `Resolution`) and
  checked by the header rules of `hv_rules.c`, the `colr` boxes of a
  `cgrp` as those of `jp2h` (in a JPX file; `cgrp.empty` without one), a
  restricted ICC profile's header and tag table; and each codestream's
  header (for a JPX file its `jpch` over the `jp2h` defaults) against its
  SIZ and main COD, where the codestream is embedded. In a JP2 file also
  the `ihdr`'s IPR against the file's IPR boxes (`ihdr.ipr`); in a JPX
  file an IPR box in a `jpch` (`jpch.ipr`), one in the file for a
  codestream whose `ihdr` has IPR 1 (`jpx.ipr`), `creg` in every `jplh` or
  none (`jpx.creg`), in a baseline file the first compositing layer's rules
  (`jpxb.*`), and in a file that lists `jp2 ` the JP2 file's rules on
  `jp2h` and the first codestream. A rule comparing boxes points at the
  codestream's header box (its `jpch`, else `jp2h`); a codestream whose
  main header the reader rejects (HV_READ_VALIDATE, up to its first SOT) fails
  with the reader's error at its offset. The
  top-level boxes are read a fixed number of times, whatever the number of
  codestreams. `hv_transcode` and `hv_merge` require `hv_check_jp2h` of
  their inputs.
- Bytes after the fields of `ihdr`, `resc`, `resd`, `cdef` and `rreq` are
  `<box>.extent` (`res.extent` for `resc` and `resd`) however many there
  are: the reader counts those of
  `cdef` and `rreq`, and decodes the other types from at most their
  largest encoding.
  The model's `Extra` holds 64 bytes (a corpus bound), so the harness
  labels a box with more `decode`.

Not read: box bodies other than the box header, `ftyp`, `flst`, `url`, a
`dtbl`'s NDR, `rreq`, the header boxes and those the box tree rules read;
the contents of the XML and IPR boxes. Unknown marker codes are reported
as items and skipped, by their length, or, from 0xFF30 to 0xFF3F, which
have no marker segment, by the marker alone; the caller decides.

## Deferred PLT traversal

`HV_READ_JPIP_INDEX` walks the served profile's codestream structure
without reading PLT entries. Copy each reported `hv_plt` descriptor and
record its tile-part's data range. The descriptor does not own the bytes.
No packet-length array is allocated. `HV_END` in this mode establishes only
structural completion. `hv_codestream_check` rejects this flag so its success
always means full validation under the requested rules.

For later packet indexing, initialize an independent `hv_plt_reader` with
`HV_READ_JPIP`. For each tile-part, `hv_plt_check_segments` checks the saved Zplt sequence
without decoding entries. Call `hv_plt_begin` for each saved segment and consume
lengths with `hv_plt_read`, passing the current buffer base on each call. For
packet indexing, `hv_plt_packet` additionally skips padding and checks early
coverage, returning a nonzero length and its offset relative to SOD data. Reading may pause and resume; only the consumed prefix has been checked.
The cursor keeps offsets, so the file may be unmapped between calls and
remapped at another address. Each buffer must contain the same unchanged file
through the saved segment end and remain alive for that read.
Nonzero lengths describe packets; zero lengths are trailing padding. When
building offsets with the lower-level `hv_plt_read`, check each length against
the remaining tile-part data before exposing the packet. `hv_plt_packet` does
that check. Its final logical packet must still be withheld until the caller
drains the suffix and completes the tile and codestream. After consuming the tile-part, call
`hv_plt_end_tile` with its data length. After the last tile-part, call
`hv_plt_end` with the expected count returned by `hv_rule_packets`. Compute
that count while SIZ and COD are alive; the headers need not be retained
through deferred indexing. These completion checks enforce coverage and total
packet count. Return values must be checked.

The cursor and the eager reader use the same generated decoder and PLT
rules. An entry is decoded once per validation traversal. On a rule error,
the cursor finishes decoding the current segment before reporting it, preserving
structural-error precedence over Zplt and entry rules. Errors are terminal
until reinitialization. Different cursors share no mutable state.

Deferred traversal is limited to the served profile. General T.800 streams
with packed headers, coding overrides or SOP retain the existing eager path.
The server uses this API through `jpeg2000::ImageIndex` in `jpip`.


The C++ index and response library is documented in [jpip/](../jpip/README.md).
