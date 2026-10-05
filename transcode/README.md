# transcode

`hv_transcode`, the C port of hvJP2K's transcoder (`jp2_transcode.py`,
`jp2_precincts.py`, `jp2_packets.pyx`). Like

    kdu_transcode Corder=RPCL ORGgen_plt=yes Cprecincts={128,128}

it rewrites the codestream in RPCL order with the given precincts and PLT
markers, without recompressing it: every code-block keeps its coding passes,
bytes and zero bit-planes, and only the packet headers change. It reads and
writes headers, and lays out precincts and packets, with `jpeg2000`
(`../jpeg2000/`: `hv_reader` with the shared rules of `hv_rules`, `hv_writer`,
`hv_geometry`).

## Usage

    hv_transcode [-p W,H] input output

- `-p W,H`: precinct width and height, powers of 2 from 2 to 32,768; default
  `128,128`.

The input is read into memory (not mapped, so that a file another process
truncates meanwhile cannot crash the tool), and one over INT_MAX bytes is
refused before it is read (`file.size-limit`). The file keeps its boxes, in
order: the `jp2c` box is transcoded, a top-level XML box ends before its
first NUL (see below), the others are copied as read. The output is
written to a temporary file next to it with the input's
permissions (without the setuid, setgid and sticky bits) and renamed into
place (`hv_file`, in `../tools/`: the temporary is removed if a signal ends
the tool first; a replaced file keeps its owner and
group where the process may give them), so input and output may be the same
file; an output that is a symbolic link is written where the link points,
the file it names created if need be. On error nothing is written. Replacement is
atomic, with no crash-durability guarantee.
The shared JP2 reader checks header, resolution and UUID-info box children.
JPX-only extension boxes remain opaque in this JP2 file and are copied whole,
without descending into their bodies. Exit status: 0, 1 on error, 2 on usage errors.

A linked JPX file (`hv_merge -links`) records where each codestream is in
its JP2 file. Transcoding such a file, in place or not, moves its
codestream: merge the JPX file again from the new files.

## Supported input

The command writes for the JPIP server, so its input must be a JP2 file within
the served profile (`../JPIP_PROFILE.md`) except for its tile-parts,
which are rewritten. The reader checks this with the rules shared with the
model (`../jpeg2000/`): the file rules of `hv_served_jp2` (signature, file type
with the `jp2 ` brand and compatibility entry, exactly one `jp2c`, at most
`INT_MAX` bytes; JPX files and raw codestreams fail) and the main-header
rules of `HV_READ_PACKETS_JPIP` (zero origins, unit sampling, one tile,
dimensions up to `INT32_MAX`, no COC, POC or PPM). The header boxes, which
the output keeps as read, are checked by `hv_read_jp2h` (T.800 I.5.3:
one `jp2h` before the codestream, its boxes well formed and in order,
and `ihdr` and `bpcc` agreeing with SIZ) against SIZ and COD MCT
read from the main header. Packet processing separately checks the full COD. This checks header consistency
without interpreting copied marker bodies. What else the profile asks
for, the transcoder writes: the tile-parts and a COD without SOP. Errors from the
reader name the rule where a shared one fails, and the offset in the file,
as in `siz.zero-origin at 410`. The output is at most `INT_MAX` bytes and
passes the whole profile (`HV_READ_JPIP`): the tests check every file they
transcode, and the fuzz target every codestream whose main header is
within the served profile.

The file wrapper preserves Rsiz under the serving reader's policy. Geometry
checks the decoded fields and tile/component layout without reapplying the
standard validator's declaration limits. This does not validate the profile
named by Rsiz; unsupported coding markers and styles still fail. The raw
entry point with `HV_OUTPUT_JPEG2000` explicitly retains its previous declaration support
limit through `hv_rule_rsiz`, before writing output. Rewriting progression and
precincts does not establish conformance to unimplemented declared profiles.
This output restriction belongs to transcode, rather than to the packet reader.

Within that, as in hvJP2K: up to 255 tile-parts, as many as TPsot can number
(Psot = 0 allowed on the last one), any progression order, only PLT and COM
in tile-part headers, and code-block styles without selective arithmetic
coding bypass or termination on each coding pass. RGN, which hvJP2K rejects,
is kept: it changes neither the packets nor their headers. The new precincts
must keep the code-block partition. SOP and EPH markers are checked and not
written. Input PLT segments must have bounded framing, but their bodies
are ignored. The transcoder reads the actual packet headers and data,
then generates new PLT lengths for the output. One rule (`main_marker`
and `tile_marker` in `transcode.c`) decides every marker, at the file and the codestream level
alike: in the main header SIZ, QCD, QCC, RGN, CRG and COM are copied as
read (COD is replaced), TLM and PLM dropped, and anything else rejected:
COC, POC and PPM, a marker code
T.800 does not define (Table A.2: 0xFF70, say, or an extension of T.801),
which may describe the packets the transcoder rewrites, and 0xFF30 to
0xFF3F, which have no segment and are outside the served profile. In
tile-part headers, PLT and COM are dropped with the headers, and anything
else rejected.

Both `hv_transcode_codestream` and `hv_transcode_file` take the same
`hv_output_profile` as merge. `HV_OUTPUT_JPEG2000` supports the transcoder's
general geometry, including nonzero origins and subsampled components.
`HV_OUTPUT_JPIP` requires the serving geometry and container restrictions.
The command-line tool selects `HV_OUTPUT_JPIP` as before.

The transcoder chooses `HV_READ_PACKETS` or `HV_READ_PACKETS_JPIP` internally.
Both check SIZ, COD, marker framing and tile-part boundaries, while copied or
dropped bodies stay opaque. Input PLT numbering, lengths and padding are not
interpreted because the packet reader derives lengths and writes fresh PLT.

The remaining restrictions have these owners:

| Requirement | Owner and reason |
| --- | --- |
| Bounded marker/tile framing, one main COD/QCD, tile-part sequencing | Packet reader: identifies the actual tile data and prevents ambiguous or out-of-bounds access. |
| SIZ component/grid fields, COD layers/levels/order/precincts and code-block partition | Packet reader and geometry: calculate and traverse the input and output layouts. COD field ranges also keep the regenerated COD representable. |
| One tile; no unsupported coding/packet-header overrides or extensions | Transcode algorithm: processes one tile using the main COD and inline packet headers. |
| No BYPASS or TERMALL code-block style | Tier-2 support: the supported contribution segmentation depends on where coding passes terminate. |
| New precincts leave nonempty code-block partitions unchanged | Output algorithm: reuses compressed code-block bytes without recompression. |
| SOP/EPH correctness and complete bounded packet contributions | Tier-2 input: reads actual packets, independent of input PLT. The output regenerates headers without SOP/EPH. |
| Memory, packet and resolution limits | Implementation bounds: control allocations for both layouts, packet lists and contribution state. |
| Rsiz declaration support with `HV_OUTPUT_JPEG2000` | Raw output policy: retains the declaration while changing packet organization. |
| Zero origins, unit sampling, serving coordinate range and allowed main-header markers | Serving output policy selected by `HV_OUTPUT_JPIP`; these are not general raw-transcode requirements. |
| JP2 brand/compatibility, header consistency and copied JP2 tree bounds | File output policy: keeps a JP2 container for the server and preserves its header information. |
| File input/output at most INT_MAX bytes | Current file implementation limit, separate from packet geometry. |

General output accepts nonzero origins and component subsampling in both raw and JP2 file operations. The sampling
regression scales a fixture's reference grid and sampling together, preserving
its component grid and compressed bytes. Its raw result differs only in those
SIZ fields; file transcode rejects the same input as `siz.component-sampling`.
This establishes the input/output policy distinction for that layout, rather
than promising support for additional coding features.

Three bounds keep memory in check where a header can declare much more
than its data holds: at most 65,536 component-resolutions (components
times resolutions, 360 bytes of layout each, checked before allocating),
at most 250,000 code-blocks (under 100 bytes of state each) and at most
2,000,000 packets in the output (20 bytes each, plus at least a byte of
output, and 64 bytes per precinct: 40 to put the packets in order, 24 to
index its tag trees). The input's packets are bounded by its tile's data,
each taking at least a byte of it, and take 12 bytes each: at most
16,000,000 (192 MB; 1,024 code-blocks in 15,625 layers take that many),
so that a file of empty packets cannot ask for 12 times its size. Each
contribution of a code-block to a layer that they signal takes 32 bytes,
and a packet header signals one in a few bits, so at most 8,000,000 are
read (250,000 code-blocks in 32 layers, 256 MB). A file beyond a bound
fails with a message that states it, as in "code-block count exceeds
supported limit (250,000)". The resolution and packet bounds are
`hv_geometry`'s (`hv_geometry_limits`), which the transcoder sets.

Packet headers are read as T.800 requires, where hvJP2K is lenient: an
SOP marker segment must have Lsop = 4 and Nsop equal to the packet's index
in the tile, modulo 65,536 (A.8.1); when COD signals EPH, every packet
header must end with it (A.8.2); and the byte after each 0xFF in a header
must have its top bit clear (B.10.1). hvJP2K skips six bytes after any SOP
code, skips EPH where it finds it, and ignores the stuffed bit. None of the
4,014 EUI files, 200 AIA files from JSOC (10 wavelengths, 2015 to 2026),
hvJP2K fixtures or test corpus files breaks these rules.

The code-block data are checked too: a code-block's contribution to a
packet must not end in 0xFF (B.10.7) or hold 0xFF followed by a byte above
0x8F, which reads as a marker (C.3.4).

Packet-level errors otherwise use hvJP2K's messages, except for the rules
above, the memory bounds, and one case: hvJP2K reads a tile's tile-parts
as one concatenated block, `hv_transcode` reads each in place. A packet in
a tile-part other than the last that runs past its tile-part is reported
as "packet crosses a tile-part boundary" even when the data after it would
also run out, where hvJP2K reports "packet data overruns the tile". Header
errors come from the reader and use its messages.

## XML boxes

An XML box holds a well-formed XML document (T.800 I.7.1), which has no
NUL character (XML 1.0, `Char`). Old Kakadu, run from IDL, ended its XML
boxes with one. jpylyzer rejects such a file, and hvJP2K's
`--xml-rewrite` was meant to remove the NUL. So every top-level XML box
ends before its first NUL, with its length set to match; a box without a
NUL is copied as read. Nothing else in the XML is read or changed: it is
not checked, and not reserialized as hvJP2K's glymur does. XML boxes
inside superboxes are copied as read with the superbox.

## Files

| File | Role |
| --- | --- |
| `hv_transcode.c` | The command: options, reading the input, writing the output file. |
| `transcode_file.c` | `hv_transcode_file`: boxes (the codestream is transcoded straight into its `jp2c` box, XML boxes end before their first NUL). |
| `transcode.h` / `.c` | `hv_transcode_codestream` (`transcode_codestream`): reads the codestream and writes its main header with the new COD, lays out the tile twice with `hv_geometry` (input and new precincts), checks the memory limits and the code-block partition, and writes the tile as one tile-part. |
| `tier2.h` / `.c` | Packets (T.800 B.9, B.10) on an `hv_geometry` and its packet order. `hv_read_packets` decodes the headers in place, tile-part by tile-part, and records each code-block's contributions per layer; `hv_write_packets` encodes them, either for the lengths only (for the PLT) or into the output, copying the code-block bytes from the input. |
| `test/` | `test_transcode.c` and `cli_test.sh` (below); `fixtures/`. |
| `fuzz_transcode.c` | libFuzzer target for the codestream level: an accepted input's output must transcode to itself, and pass `HV_READ_JPIP` when its main header passes `HV_READ_PACKETS_JPIP`. |

## Build and test

With the rest of the repository, so configuring needs the server's
dependencies too: the top-level `CMakeLists.txt` also adds `server/`, which
requires zlib, glib and llhttp.

```sh
cmake -S . -B build [-DESAJPIP_SANITIZE=ON]
cmake --build build --target hv_transcode
```

Fuzzing (Clang):

```sh
cmake -S . -B fuzz -DCMAKE_C_COMPILER=clang -DESAJPIP_SANITIZE=ON -DESAJPIP_FUZZ=ON
cmake --build fuzz --target fuzz_transcode
fuzz/tests/fuzz/fuzz_transcode -max_len=131072 corpus/
```

Tests, through the shared runner:

```sh
tests/run.sh             # or: tests/run.sh sanitize
TRANSCODE_ARCHIVE=~/AIA:~/EUI tests/run.sh
```

CTest options follow an explicit mode, for example
`tests/run.sh normal -R transcode`. With no arguments the runner executes the
whole suite normally. Builds go to `build/tests-<mode>`, or to
`ESAJPIP_TEST_BUILD_DIR`.

`test_transcode` checks that every file in `../tests/transcode/fixtures/input/` transcodes
to its Kakadu reference in `../tests/transcode/fixtures/kakadu/` (COM and the XML box
aside: it is the input's, before its NUL), that each output is within the
served profile and transcodes to itself (the origin-129 file is rejected,
and only its codestream is compared); what the profile accepts and rejects;
the boxes around the codestream (LBox = 0 on a superbox and its child, a
malformed JP2 child, opaque JPX extension bodies and nesting, XML boxes cut
before their first NUL or copied as read);
tile-parts split every way T.800 allows and broken in the ways it does not;
malformed main headers; 2,000 corrupted tiles, each rejected or giving a
stable output; the SOP, EPH, bit-stuffing and code-block data rules; the
memory bounds (65,536 component-resolutions and 250,000 code-blocks at
their edge, and 16,000,000 input packets, 2,000,000 output packets and a
lowered contribution limit exceeded); the code-block partition changed at
one resolution only, at the first or the last, in a band of one
code-block, or only in empty bands; the
precinct sizes at their limits; the segments dropped, kept and rejected in
each header; 255 tile-parts and a 256th; two tiles. `cli_test.sh` checks the
command: options, exit status, in-place replacement keeping the mode, output
through a symbolic link, and nothing written or left behind on failure. With
`TRANSCODE_ARCHIVE` set to directories, every `.jp2` file in them is
transcoded and checked for a stable output too. The fixtures are described
in `../tests/transcode/fixtures/FIXTURES.md`. The same run includes the reader's tests
(`../tests/jpeg2000/`).
