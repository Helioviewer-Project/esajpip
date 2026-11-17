# Changelog

## 2.0-rc1 - Unreleased

Version 2.0 rebuilds the server, adds a JPIP client with its own decoder, and
adds tools that check and prepare JP2 and JPX files for serving.

The server is now a single foreground process built on Asio. A control thread
accepts connections and routes requests, one I/O thread per hardware thread
generates and writes responses, and a small pool opens images. A JPIP channel
is no longer tied to one TCP connection, so connections can be pooled or
replaced without losing the channel.

### Upgrading from 1.9

- Configuration moves from `server.cfg` to `server.ini`. Rewrite existing
  configurations; `README.md` lists every setting.
- `esajpip` runs in the foreground and takes no command-line options. Restart
  it with a process manager or a container runtime. The supervisor process,
  the `status` command and the shared-memory process registry are gone.
- Clients must use HTTP/1.1.
- Log files are named `esajpip.<address>.<port>.<timestamp>.log`. They roll
  at 1 GiB and keep one backup.
- File logging is off unless `logging.file_enabled` is `true`; CMake turns
  it on when `ESAJPIP_LOG_DIRECTORY` is set.
- Building the server needs C11 and C++11 compilers, CMake 3.20,
  pkg-config, GLib, llhttp, zlib and POSIX threads; Debian 13 is the minimum
  supported Debian release. A client-only build
  (`-DESAJPIP_CLIENT_ONLY=ON`) needs none of pkg-config, GLib, llhttp or
  zlib. libconfig, log4cpp and libgsf are no longer needed.
- Images are checked against the served profile (`JPIP_PROFILE.md`) when they
  are opened. Files without PLT markers, with more than one tile, or with
  markers the server cannot index are rejected instead of failing later.
  `hv_transcode` and `hv_merge --transcode` add PLT markers to single-tile
  files the transcoder can read (`transcode/README.md`); other files must be
  re-encoded.
- Existing movies from hvJP2K's `hv_jpx_merge` and the deployed `kdu_merge`
  form still load. JPX-to-JPX links and raw codestream files are rejected.

### Server

- Channels survive a replaced connection, and one persistent connection can
  carry requests for several channels, so browsers and reverse proxies can
  pool connections. Channel IDs are random and cannot be guessed.
- Connections are screened before any JPEG 2000 state is allocated. A
  connection that does not send a JPIP request within
  `connections.initial_timeout` is closed; identified connections use
  `connections.timeout`. Connections and channels have separate limits.
- Each response is generated chunk by chunk on the thread that writes it.
  Different connections use different cores, and a slow client slows only
  its own response. gzip output is streamed instead of buffered.
- New JPIP support: the `layers` field; PCRL and CPRL progression, so all
  five orders are served; one request spanning frames that differ in size,
  decomposition levels, quality layers or precincts; open-ended `context`
  and cache-model ranges; standard codestream lists; and absolute request
  URIs. A request that asks for `handled` receives a conservative
  `JPIP-handled` header.
- Errors are reported with standard HTTP status codes (400, 404, 431, 500,
  501, 503) and a short description. `JPIP-tid` no longer exposes file
  paths.
- Each channel's image index and cache model take less memory.

### Client

- `client/`: a new JPIP client for the images and movies the server sends.
  The C library keeps everything it receives, works out what a frame still
  needs at the chosen resolution and quality, rebuilds the codestream, and
  supplies the frame's XML metadata and color table. The host program
  makes the HTTP requests and decodes the pixels.
- The same library, with the vendored OpenJPEG decoder, runs as WebAssembly
  behind a JavaScript module and a Web Worker and returns 8-bit pixels. A
  demonstration page plays movies against a running server.
- Native programs can open local JP2 and JPX files through the same frame
  interface (`hvc_open_local`).
- Frames can be previewed with one quality layer and refined later, and are
  requested at the size the display needs.
- After a dropped connection or a lost channel, the client opens a
  replacement channel from its cache and retries, keeping what it already
  has. Frames can be exported to and imported from a disk cache.
- Native hosts such as JHelioviewer can also use it with 1.x servers
  (`client/CLASSICAL.md`).
- The bundled OpenJPEG decoder is faster than upstream OpenJPEG and can
  decode a region. It uses WebAssembly SIMD, and in native builds NEON on
  ARM64 and up to eight worker threads.

### Tools

- `hv_walk` checks JP2 and JPX files against the standard and the served
  profile, and raw codestreams against the standard.
- `hv_transcode` rewrites a JP2 file in RPCL order with the given precincts
  and PLT markers, without recompressing, so the server can index it. It
  replaces hvJP2K's transcoder.
- `hv_merge` merges JP2 files into a JPX movie, with the codestreams copied
  or linked (`-links`), and replaces hvJP2K's merger. `--validate` checks
  every input against the served profile. `--transcode` transcodes each
  input in memory and embeds it, so archives without PLT markers can be
  served without being modified or copied. No merge may overwrite one of
  its inputs. Options use their exact names; hvJP2K's abbreviations and
  attached values are not accepted, and the words of an `-s` file are
  inserted where `-s` stands instead of being parsed again.
- `hv_transcode` and `hv_merge` write to a temporary file and rename it into
  place, so a failed run leaves an existing output unchanged.

### Libraries

- `jpeg2000/`: a JPEG 2000 reader and writer for boxes and codestreams,
  shared by the server, the client and the tools. Its rules follow a formal
  model of the accepted files in `spec/`, which also generates a labeled test
  corpus.
- `jpip/`: JPIP request parsing, image indexing and response generation,
  separate from the server's network code.

### Development

- The test suite is opt-in (`BUILD_TESTING`) and runs with `tests/run.sh`.
  Sanitizer, coverage, Valgrind, Debian-container and mutation profiles are
  described in `tests/DIAGNOSTICS.md`.
- libFuzzer targets cover the JPEG 2000 library, the tools, the server's JPIP
  code and the client (`ESAJPIP_FUZZ=ON`).
- Optimized builds use link-time optimization where the compiler supports it.

### Fixed

- Request fields no longer carry over from an earlier request on the same
  channel.
- Response limits, cache models and metadata continuation work across
  requests. An omitted `len` leaves only the server's 2 GiB response
  budget, small limits still end with an EOR message, and metadata resumes
  correctly inside JPX placeholders.
- Window requests follow the standard: default values, the resolution
  mapping, and every precinct the window touches. A window outside the image
  returns `EOR WINDOW_DONE` instead of ending the channel.
- Embedded JPX codestreams keep their own coding parameters, use the standard
  default precinct size, and are numbered in file order.
- Values a client controls are bounded before memory is allocated, including
  cache models, response limits, codestream selections and request heads.
- Request paths containing `..` are rejected. Links stored in JPX files may
  still point outside the image directory.
- Logs name the linked JP2 file that stops a JPX from loading, and shutdown
  writes out queued log records.

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
