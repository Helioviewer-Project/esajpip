# Changelog

## 2.0-rc1 - Unreleased

Version 2.0 rearchitects the server. Where 1.x used a parent and a serving
child, passed accepted sockets between them, created a detached thread per
client, and tied a JPIP channel to one TCP connection, 2.0 is a single
foreground process built on Asio: a control thread owns admission, channel
routing, and signals; each connection parses its requests and generates and
writes its responses on a bounded set of I/O threads; and a separate pool opens
images. Each channel keeps private image, cache, and traversal state and is
used by one thread at a time, which isolates JPEG 2000 state while letting
HTTP connections be pooled or replaced independently of channels.

### Added

- `spec/`: the accepted JP2 and JPX files described in ASN.1/ACN, at two
  layers (T.800/T.801 and the served profile), with a generated, labeled
  test corpus that the server's tests and the tools' tests check against.
  `spec/check-model.sh` checks the model, the corpus and the C code that
  restates the model, and that OpenJPEG decodes every standard-valid vector
  but those on the OpenJPEG limits it lists. The pinned asn1scc is built
  from unmodified upstream 4.9.7.0; the eleven local fixes for issue #417
  are retired in `spec/asn1scc-patches/reference/161cc246/`.
- `jpeg2000/`: a JPEG 2000 reader/writer (`jpeg2000`) on code generated from
  that model, applying the same cross-field rules as the corpus
  (`hv_rules`). It reads boxes and codestream items in place, one header or
  list element at a time, checks the served profile, the JP2/JPX header
  boxes, the box tree and its fragment tables, JP2 compatibility, baseline
  JPX, the marker segments across the codestream headers, and the files a
  JPX file links to, lays out a tile's precincts and packets
  (`hv_geometry`), and writes boxes and marker segments with measured
  lengths. Its work is in proportion to
  the input, whatever a header declares. `hv_walk` checks files with it.
- `hv_transcode` (`transcode/`): a C port of hvJP2K's transcoder. It
  rewrites a JP2 file in RPCL order with the given precincts and PLT,
  without recompressing, and writes only files within the served profile:
  its input must be within it but for the tile-parts. It reads packet
  headers as T.800 requires (SOP, EPH, bit stuffing), rejects code-block
  data that ends in 0xFF or holds 0xFF followed by a byte above 0x8F (T.800
  B.10.7, C.3.4), bounds its memory by the input, and ends each top-level
  XML box before its first NUL: old Kakadu, in IDL, ended XML boxes with
  one, which is not XML.
- `hv_merge` (`merge/`): a C port of hvJP2K's JPX merger, writing the same
  bytes but for the differences `merge/README.md` lists, embedded or linked
  (`-links`), with hvJP2K's command line. Its inputs must be within the
  served profile and have valid header boxes and main header; it rejects
  what the JPX file cannot hold (a box T.801 places elsewhere, such as a
  `cgrp` or `cref` in `jp2h`, `colr.one-method`, `colr.approx`, a later
  input without the `cdef` or `res` box of the first) and an input
  codestream with T.801 extensions, which a JP2 file cannot hold
  (`jp2.rsiz`). An input's IPR boxes, at the top level and in its `jp2h`, go
  into its `jpch`, where hvJP2K drops them but keeps the IPR 1 announcing
  them. Every box it copies from an input goes straight to the output file,
  so its memory does not grow with them. Every input is opened once;
  the first stays mapped for header comparisons, and each later input is
  checked and emitted before closing it. Reader Requirements are patched
  into a 44-byte reservation with a zero-filled `free` box after `rreq`.
  Library output must be seekable; partial output must be discarded on failure.
  `--transcode` transcodes every input once in memory and embeds it, with
  `-p W,H` selecting precinct sizes (default 128,128), so PLT-less archives
  can be served without modifying or duplicating them. `-links` and
  `--validate` cannot be combined with `--transcode`.
- Both tools replace their output through a synced temporary file, removed
  if a signal ends the tool (SIGBUS from a truncated input included), keep
  a replaced file's owner and group, write through an output that is a
  symbolic link, and refuse inputs over `INT_MAX` bytes. The output takes
  the permissions, less the setuid, setgid and sticky bits, of the file it
  replaces (`hv_merge`) or of the input (`hv_transcode`). Every `hv_merge`
  mode refuses to replace an input through any name, hard link or symbolic
  link. hvJP2K truncates such an input before copying it, losing it and
  writing a corrupt or truncated movie.
- Every test in one place, `tests/` with its own CMake: the library and the
  tools, their command lines, the server, and the shared corpus. One gate,
  `BUILD_TESTING`, and one runner, `tests/run.sh`, with labels for `tools`,
  `server`, `cli`, `model` and `fuzz`.
- `tests/fuzz/`: the libFuzzer targets of the library and the tools
  (`ESAJPIP_FUZZ=ON`), whose assertions are also available one input at a time
  through `tests/fuzz/replay`, which needs no libFuzzer runtime. `fuzz_asn1`
  decodes and encodes every PDU type of the generated code, read from its
  headers at configure time, and checks that decoded values round-trip to the
  same bytes and that no prefix of an encoding is read past; `fuzz_merge`
  merges embedded and linked; `fuzz_deferred_plt` reads the PLT cursor from
  two copies of the input in turn.
- `tests/run_profile.sh`, `tests/run_baseline.sh`,
  `tests/run_linux_docker.sh` and `tests/run_mutation.sh`: the sanitizer,
  coverage, Valgrind, Debian-container and mutation profiles of the same
  suite, described in `tests/DIAGNOSTICS.md`, with the coverage gaps in
  `tests/COVERAGE_GAPS.md`.

### Changed

- Keep channels alive across replacement connections, and allow one persistent
  connection to carry requests for different channels. This permits ordinary
  browser and reverse-proxy connection pooling. Random, opaque channel IDs
  prevent one client from guessing another client's channel.
- Identify JPIP traffic before allocating JPEG 2000 state. Silent or unrelated
  connections retain only their sockets and expire at the short initial
  timeout. Physical connections and JPIP channels have independent limits.
- Generate each response chunk on the thread that writes it, and only once
  the socket has accepted the previous one, so responses for different
  connections use different cores and a slow client throttles generation.
- Reduce per-channel indexing memory. On the 4,014-frame linked-JPX fixture,
  resident memory at the same held point fell from 13,040 KiB to 8,976 KiB
  (31%), with CPU and elapsed time unchanged within measurement noise.
- Stream gzip output one chunk at a time instead of buffering the complete
  compressed response, removing libgsf and its dependencies.
- Replace `server.cfg` and libconfig with `server.ini`, parsed by GLib. Existing
  configurations must be rewritten for 2.0.
- Replace log4cpp with bounded asynchronous logging. Log-file I/O does not run
  on the serving threads. Logs retain timestamps, optional request
  lines, 1 GiB rotation, and one backup.
- Leave process restart to the host process manager or container runtime. The
  supervisor, `status` command, and shared-memory process registry are removed.
  Log names include the listening address and port.
- Add verified PCRL and CPRL packet traversal and allow one multi-codestream
  request to cover frames with different dimensions, decomposition levels,
  quality layers, or precinct layouts.
- Require HTTP/1.1, matching the chunked responses used by the server and the
  requests sent by JHelioviewer.

### Fixed

- Compute precinct data-bin offsets incrementally so response generation stays
  linear in packet count for sources with many quality layers.
- Keep parsed values local to one request, preventing omitted window, response,
  or rounding fields from inheriting values from an earlier request.
- Make response limits, cache models, and metadata continuation reliable across
  requests. An omitted `len` is unlimited, small limits still produce an EOR,
  and metadata resumes correctly inside JPX placeholders.
- Apply the standard window defaults and resolution mapping, including every
  precinct intersecting the adjusted window. An empty intersection returns
  `EOR WINDOW_DONE` instead of failing the channel.
- Parse standard JPIP codestream lists, closed and open ranges, sampling
  factors, cache-model qualifiers, and absolute request URIs consistently.
- Validate JPEG 2000 boxes, markers, tile-parts, PLT coverage, packet locations,
  codestream bounds, and linked-JPX references. Invalid or unsupported sources
  are rejected instead of failing later or producing inconsistent JPP data.
  Deployed trailing zero PLT entries remain accepted as padding; nonzero extras
  are rejected. QCD and COM lengths and the COM registration value are checked
  against T.800 (Lqcd 4 to 197, Lcom at least 5, Rcom 0 or 1). COC, POC,
  PPM, markers T.800 places elsewhere, 0xFF00 and the segment-less 0xFF30 to
  0xFF3F are rejected in the main header, and PLT entries longer than ten
  bytes are rejected. A tile-part with Psot = 0 runs up to the final EOC,
  without its packet data being scanned for markers.
- Preserve separate coding parameters for embedded JPX codestreams, use the
  standard default precinct size, and number codestreams by physical box order.
- Bound client-controlled request values before allocation, including cache
  models, response limits, codestream selections, and HTTP request heads.
- Return useful `400`, `404`, `431`, `500`, and `503` responses for malformed
  requests, oversized heads, missing or invalid targets, server failures, and
  unavailable channels.
- Reject client paths containing a parent-directory segment and stop exposing
  resolved filesystem paths through `JPIP-tid`. Trusted links stored inside JPX
  files may still refer to sources outside the image directory.
- Improve cleanup and diagnosis. Logs identify a linked JP2 file that prevents
  its JPX from loading, parser and response failures release owned resources, and
  orderly shutdown drains queued logs.

## 1.9.0-rc1 - 2026-09-14

### Changed

- Partition top-level JPX association contents into separate metadata bins using
  JPIP placeholders.
- Deliver linked-JPX metadata as larger contiguous data-bin messages and resume
  traversal where the previous response stopped. A representative 2 MiB
  response fell from 261 messages to 32, reducing initial loading of the
  4,014-frame JHelioviewer fixture from 77.27 seconds to 35.94 seconds. Avoiding
  repeated traversal also reduced median local server-and-transfer time from
  305.9 ms to 288.7 ms (5.7%). The 100-frame fixture remained effectively
  unchanged at 0.58 versus 0.56 seconds.
- Release linked-file mappings after indexing and after completing each
  response. For 4,014 links, warm-open peak RSS fell from about 275 MB to 81 MB.
  After 1,000 randomized requests, final RSS fell from about 787 MB to 25 MB.
- Add a conventional out-of-tree CMake build and installation.

### Fixed

- Preserve linked-JPX codestream order when resolving frame URLs.
- Handle partial socket writes when sending chunked responses.
- Accept a JPIP cache-model descriptor at the end of a query string.
