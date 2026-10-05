# merge

`hv_merge`, the C port of hvJP2K's JPX merger (`hv_jpx_merge`,
`jpx_merge.pyx`, `jpx_common.pyx`). It merges JP2 files into a JPX movie
for JHelioviewer and the JPIP server, embedding their codestreams or
linking to them, and writes the same bytes as hvJP2K but for the
differences below. It reads the inputs with `jpeg2000` (`../jpeg2000/`:
`hv_reader` with the shared rules of `hv_rules`), and writes the boxes it
makes itself (`ftyp`, `rreq`, a generated `cmap`, `nlst`, `flst`, `url`,
`dtbl`) with `hv_writer`, from the model's types.

## Usage

    hv_merge -i jp2file1,jp2file2,... [jp2file ...] -o jpxfile [-links] [--validate] [-s argfile]

The options are hvJP2K's, with an optional validation mode:

- `-i`: the input JP2 files, in the order of the JPX codestreams, comma or
  space separated (empty names between commas are dropped).
- `-o`: the output JPX file.
- `-links`: link to the codestreams (`ftbl`, and `url` boxes naming each
  input by absolute, resolved path) instead of copying them (`jp2c`).
- `-s`: more arguments from a file, or from standard input for `-`, split as
  Python's `shlex.split` does (quotes and backslashes).
- `-h`: the usage, on standard output.
- `--validate`: also validate the complete codestream against the served
  profile, including PLT entries, padding, packet count and coverage.
  Without it, the codestream after the checked main header is copied or
  referenced without validation, as in hvJP2K. The output bytes are identical
  for inputs accepted by both modes.

They are parsed as hvJP2K's `argparse` parser does. With `-s`, the command
line and the file's words after it are parsed again, once: a later `-i`
or `-o` replaces an earlier one, the command line's included; a `-s` in
the file is parsed but not read; words in the file continue a `-i` that
ends the command line, and are an error otherwise. `-i` takes the words up
to the next one that looks like an option: one starting with `-`, other
than `-` alone, a negative number such as `-1`, or a word with a space
that names no option. An option's value may be attached (`-ofile.jpx`,
`-o=file.jpx`, `-i` then taking that one name), and an option may be
abbreviated (`-lin`). `--` is an error. Names that leave nothing once the
commas are dropped fail with hvJP2K's "no JP2 input files". `cli_test.sh`
checks these cases. Deliberate differences: the argument file is read as
bytes, where hvJP2K decodes it as text and fails on bytes the locale's
encoding rejects; a NUL byte in it is an error (it would end the text
early); and `-h` prints hv_merge's own usage text.

Inputs are immutable. Their bytes and sizes must remain unchanged throughout
both passes, including between unmapping and reopening. Mutation is a
configuration/operational error outside the merger's contract. With `-links`,
the inputs must also remain unchanged for as long as the resulting JPX is used.

The inputs are read in two passes, as hvJP2K does. The first input is
checked once and stays mapped throughout. Each later input is checked,
unmapped, then mapped again to write the output using its recorded layout.
The first pass uses `hv_read_jp2` for ordinary embedded inputs and
`hv_served_jp2` for linked or validated inputs. Both paths read header information
with `hv_read_jp2h_boxes`: SIZ and COD MCT supply header/channel consistency;
other COD fields remain opaque. Copying mode does not validate the SIZ
tile grid. Rsiz admission is checked separately for Reader Requirements.
Ignored input header fields are normalized when writing. Dropped top-level
box bodies are not validated. Copied boxes are checked in their output
placement; IPR availability uses the boxes that will be emitted. Input size
and header count/order remain explicit merge restrictions. Reader Requirements
use the source's SIZ declaration, read without opening another codestream reader. With `--validate`, merge additionally validates the whole
codestream with `HV_READ_JPIP`. The second pass does not repeat validation or compare source
properties. At most two inputs are
mapped at a time. Mapping reads only what is used: the checks and a linked
merge touch the headers, an embedded merge reads each codestream once. Every
box copied from an input, the codestream, header, IPR and XML boxes alike,
is written straight from the mapping to the output file, a superbox's length
counted first; only box headers and what `hv_merge` makes itself (`ftyp`,
`rreq`, a generated `cmap` of at most 16,384 entries, `nlst`, `flst`, one
`url` at a time) pass through memory. An input that another process
truncates meanwhile raises SIGBUS when the lost bytes are read, which ends
`hv_merge` with a message; the temporary output is removed first, so the
output is left as it was. With `-links`, the second open (the first input's
only one) supplies the header, IPR and XML boxes; the codestream stays in
the input JP2 file. The output is
written to a temporary file next to it and renamed into place (`hv_file`, in
`../tools/`: the temporary is removed if a signal ends the tool first); on error nothing is written. An existing output keeps its
permissions (without the setuid, setgid and sticky bits), and its owner
and group where the process may give them; one that is a symbolic link is
written where the link points, the file it names created if need be, as
when hvJP2K opens it for writing. Exit status: 0 (also for `-h`), 1 on
error, 2 on usage errors. Output replacement is atomic, with no crash-durability
guarantee.

A linked JPX file records each input's path and its codestream's offset
and length, so the inputs must stay as they are: rewriting one, as
`hv_transcode` does in place, moves its codestream and breaks the link.
For the same reason a linked merge cannot write its output over one of
its inputs (the same file by any name or link is an error); an embedded
merge can. Merge again after changing an input.

## The output

As hvJP2K writes it:

- the signature, `ftyp` (brand `jpx `, MinV 1, compatible with `jpx `,
  `jp2 ` and `jpxb`, or `jpx ` alone when linked; the model's `FtypHeader`
  and `Brand`) and `rreq`, written with the model's encoders one part at
  a time (`hv_write_rreq`: `RreqHeader` with ML 1, then each
  `RreqStandardFeature`, and no vendor features; T.801 M.11.1, Table
  M.14: feature 1, 2 for more than one codestream, 4 and 5 from the
  codestreams' Rsiz (Profile 1, a T.800 codestream without profile; an
  input's Rsiz is restricted to 0, 1 or 2 by the Reader Requirements this
  merger writes, not by the full T.800 Table A.10), 9 and 10 for opacity
  channels in `cdef`, 15 when linked);
- `jp2h`, the first input's, with each `colr`'s ignored PREC and APPROX
  emitted as 0 and 1;
- for each input, a `jpch` and a `jplh`: when its `jp2h` is the first
  input's, empty but for the IPR boxes below, otherwise the `ihdr` and
  whatever of `bpcc`, `pclr`, `cmap` (generated where the first input has
  a palette and this one has none), `colr` (in a `cgrp`), `cdef` and `res`
  differs from the first; and in the `jpch`, the input's IPR boxes
  (`jp2i`), at the top level and in its `jp2h`, which its `ihdr`'s IPR 1
  announces, so that the codestream's IPR 1 has one (`jpx.ipr`);
- the codestream, as `jp2c` or as an `ftbl` holding one `flst` fragment;
- its first XML box, if any, in an `asoc` with an `nlst` naming codestream
  and compositing layer i (`hv_write_nlst`, the model's `NlstEntry`);
- when linked, a final `dtbl` with one `url` per input.

## Supported input

Ordinary embedded merging uses `hv_read_jp2` to discover one bounded codestream
in a JP2-compatible input. Linked and validated merging use `hv_served_jp2`,
which additionally requires the server's primary brand and file-size policy.
Merge itself limits input size to `INT_MAX`, requires one `jp2h` before `jp2c`,
and reads that selected pair through `hv_read_jp2h_boxes`. Header image/component
fields must agree with bounded SIZ fields, and JP2 channel checks use COD's MCT.
Copying mode leaves other coding fields uninterpreted and does not apply SIZ
tile-grid rules. Rsiz 0/1/2 admission
belongs to the Reader Requirements that merge generates. Only `--validate`
adds complete served codestream checks through `HV_READ_JPIP`.

The first input's header is copied into the output; later headers supply image,
palette, channel and resolution differences or inherit the first header's
values. Their interpreted fields and copied tree placement therefore support
output correctness. Dropped top-level bodies and later unused header extensions
are not validated. Flat `asoc(nlst, xml)` metadata and intact codestream bytes
are emitted in input order.

The output container passes `hv_served_jpx` in both modes. Only validation mode
also requires every codestream to pass `HV_READ_JPIP`; default mode does not
promise that the codestream can be served or decoded. The
tests check both of the JPX files they write, but for the Reader
Requirements cases and `test_opening`'s, and the fuzz target of every
merge it accepts. Three
things the selected JP2 header reader accepts in a JP2 file are invalid in the JPX file,
and are rejected: a box that T.801 places elsewhere, in the first input's
`jp2h`, which is the JPX file's, or in a later input's `res`, which goes
into its `jplh` (`hv_rule_box_placed`: a `cgrp`, `cgrp.placement`, T.801
M.11.7.1, or a `cref`, `cref.placement`, M.11.4, which a JP2 reader skips
as a box T.800 does not define; a later input's `cgrp` is not copied);
more than one `colr` box with METH 1, or more than one with
METH 2, in an input, whose `colr` boxes become the JPX file's `jp2h`'s (the
first input) or a `cgrp`'s (a later input whose colors differ from the
first's), where T.801 M.11.7.1 allows no more (`colr.one-method`); and a
later input without a `cdef` or `res` box after a first input with
one, as its `jplh` would inherit the first input's from `jp2h` (T.801
M.11.7). hvJP2K checks neither the profile, nor the header boxes, nor the
link targets, nor these three. Both hvJP2K and default `hv_merge` can merge
files without PLT; the server rejects those. Use `--validate` to require
servable codestreams during merging.

Errors about an input name the file and, where a shared rule fails, the
rule and its offset, as in `siz.zero-origin at 410`. The others are
described in words: a missing `cdef` or `res`, too many `colr` boxes, and
the limits. The output must be
at most `INT_MAX` bytes (`file.size-limit`), and a link must name a `.jp2`
file (`url.jp2-target`). Limits: an input's `jp2h` holds at most 16 `colr`
boxes, a JPX file at most 16,777,215 inputs, and a linked one at most
65,535.

Differences from hvJP2K, beyond these checks: an XML box running to the
end of its file (LBox = 0) gets an explicit length inside the `asoc`,
where hvJP2K would copy the zero; every box copied from `jp2h` gets a
fresh header, as glymur writes it, where only an XLBox form would differ;
and an input's IPR boxes are carried into its `jpch`, where hvJP2K drops
them and keeps the IPR 1 that announces them.

## Files

| File | Role |
| --- | --- |
| `hv_merge.c` | The command: options, `-s`, mapping the inputs, writing the output file. |
| `merge.h` / `.c` | `hv_merge_files`: checks the inputs, and writes the JPX file box by box to a stream, opening each input when it needs it (`hv_merge_buffers` for inputs already in memory). |
| `test/` | `test_merge.c`: hvJP2K's output byte for byte (`fixtures/`), the linked merge box for box against it and within the served profile, the reader requirement cases of hvJP2K's tests, IPR boxes carried into each `jpch`, an XML box with LBox = 0 given a length, rejected inputs (Rsiz bit 15, header boxes, `colr` counts and methods, the limits), inputs opened on demand (`test_opening`: how often each is opened, at most two at a time, a failed first or second open), and 1,024- and 1,025-character links; `cli_test.sh`: the command (options as `argparse` parses them, `-s`, permissions, symbolic links, `-links` over an input, failures). |
| `fuzz_merge.c` | libFuzzer target: two JP2 files from one input, merged in both modes; an accepted merge must pass `hv_served_jpx` and `hv_check_jpx_headers`, plus `HV_READ_JPIP` for each codestream in validation mode. |

## Build and test

With the rest of the repository, so configuring needs the server's
dependencies too: the top-level `CMakeLists.txt` also adds `server/`, which
requires zlib, glib and llhttp.

```sh
cmake -S . -B build [-DESAJPIP_SANITIZE=ON]
cmake --build build --target hv_merge
```

Tests, through the shared runner:

```sh
tests/run.sh             # or: tests/run.sh sanitize
```

CTest options follow an explicit mode, for example
`tests/run.sh normal -R merge`. With no arguments the runner executes the
whole suite normally. Builds go to `build/tests-<mode>`, or to
`ESAJPIP_TEST_BUILD_DIR`.

Fixtures are described in `../tests/merge/fixtures/FIXTURES.md`.

Fuzzing (Clang):

```sh
cmake -S . -B fuzz -DCMAKE_C_COMPILER=clang -DESAJPIP_SANITIZE=ON -DESAJPIP_FUZZ=ON
cmake --build fuzz --target fuzz_merge
fuzz/tests/fuzz/fuzz_merge -max_len=262144 corpus/
```

## Performance check

Measured on macOS arm64 with a Release build, without LTO, on 2026-09-28.
Medians of nine runs after warm-up, alternating implementations with a warm
filesystem cache. Times are milliseconds. C timings include CLI startup and
the file/directory synchronization used at that revision (since removed);
hvJP2K timings are warm Cython library calls.

| Input and output | Before | Default | `--validate` | hvJP2K |
| --- | ---: | ---: | ---: | ---: |
| 100 HRI files, linked | 28.47 | 8.26 | 19.94 | 5.40 |
| 100 HRI files, embedded | 49.19 | 29.30 | 40.39 | 27.20 |
| 4,014 EUI files, linked | 893.42 | 244.75 | 623.73 | 239.05 |

All outputs were byte-identical across these four implementations. Default
mode avoids PLT traversal; validation mode decodes each entry once per validation
pass, retaining both input passes and diagnostic priority. Embedded timings
include copying the compressed data and vary more with output I/O.
