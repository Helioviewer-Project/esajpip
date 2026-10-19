# Running the tests

Every test of the project is under `tests/`, with its CMake; the component
`CMakeLists.txt` files build libraries and tools only.

```
tests/
  CMakeLists.txt   the gate (BUILD_TESTING) and the one registration helper
  run.sh           builds and runs the suite
  run_mutation.sh  mutation testing, with mutate.py making the mutants
  jpeg2000/        jpeg2000: reader, rules, writer, hv_rewrite, geometry,
                   hv_file, hv_served, hv_walk
  jpip/            jpip: source/, index/, request/, response/ (see jpip/README.md)
  transcode/       hv_transcode: the library and its command line (fixtures/)
  merge/           hv_merge: the library and its command line (fixtures/)
  server/          storage, channel, HTTP and live-server tests (see server/README.md)
  fuzz/            corpus replay, plus libFuzzer targets with ESAJPIP_FUZZ=ON
  vectors/j2k/     the corpus and its manifest, shared by reader and server
```

From the repository root:

```sh
./tests/run.sh
./tests/run.sh sanitize
./tests/run.sh normal -L tools
ESAJPIP_TEST_TARGETS=jpip_tests ./tests/run.sh normal -L '^jpip$'
ESAJPIP_TEST_TARGETS=server_tests ./tests/run.sh normal -L '^server$'
./tests/run.sh normal -N                   # list without running
```

For an 8-core local run, limit the build and run CTest in parallel:

```sh
ESAJPIP_JOBS=8 ESAJPIP_CTEST_JOBS=8 ./tests/run.sh normal
ESAJPIP_JOBS=8 ESAJPIP_CTEST_JOBS=8 ./tests/run.sh sanitize
```

The runner configures, builds and runs CTest. Normal and ASan/UBSan builds use
separate directories under `build/`, so neither changes your installation
build. It needs Python 3 and the same compiler and libraries as the server,
but no external image collection, running server, JHV, Docker, or ASN.1 compiler.
Tests create their own files and use loopback sockets.

Options:

- `ESAJPIP_TEST_BUILD_DIR` — another build directory.
- `ESAJPIP_TEST_TARGETS` — build only some targets, to iterate quickly.
- `ESAJPIP_JOBS` — build parallelism for all test runners.
- `ESAJPIP_CTEST_JOBS` — CTest parallelism for runners that invoke CTest.
- `CMAKE_BUILD_PARALLEL_LEVEL` and `CTEST_PARALLEL_LEVEL` — standard CMake and
  CTest equivalents, used when the `ESAJPIP_*` variables are not set.
- `ESAJPIP_FUZZ=ON` — also build the fuzz targets (the compiler then needs the
  libFuzzer runtime).
- `BUILD_TESTING` (default OFF) gates every test. The runners enable it explicitly.

CTest labels: `tools` (jpeg2000, jpip and the two tools), `server`, `client`,
`cli` (the two command-line tests), `model` (`model_static`) and `fuzz` (the
fuzz targets).
The JPIP library also has the `jpip` label and responsibility labels described
in [its test guide](jpip/README.md). The server has `server_tests` and
responsibility labels described in [its test guide](server/README.md).
For a manual test build:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

For an already built tree configured with `BUILD_TESTING=ON`, the CTest command
is sufficient.

## The other runners

`run.sh` is the common case; the rest run the same tests under another
toolchain, and are described in [DIAGNOSTICS.md](DIAGNOSTICS.md):

| Runner | What it does |
| --- | --- |
| `tests/run.sh [normal\|sanitize]` | The suite, normally or under ASan and UBSan. |
| `tests/run_profile.sh asan\|extended\|msan\|optimized` | The suite under that sanitizer or in `Release`. |
| `tests/run_profile.sh fuzz` | The fuzz targets under `tests/fuzz`, then seeded `ctest -L fuzz`. |
| `tests/run_profile.sh fuzz-asan\|fuzz-extended` | Local libFuzzer mutation runs under sanitizer profiles. |
| `tests/run_profile.sh valgrind [file...]` | `reader-rewrite` under Memcheck. |
| `tests/run_baseline.sh` | The suite with Clang source coverage; [COVERAGE_GAPS.md](COVERAGE_GAPS.md) reads its reports. |
| `tests/run_linux_docker.sh valgrind\|msan\|all` | The replay modes under Valgrind and MSan, in Debian 13. |
| `tests/run_linux_docker.sh fuzz-asan\|fuzz-extended\|fuzz-msan\|fuzz-all` | Linux libFuzzer mutation runs under sanitizer profiles. |
| `build/tests-*/tests/fuzz/replay MODE FILE_OR_DIR...` | One fuzz target's assertions, with no libFuzzer runtime. |
| `tests/run_mutation.sh [file...]` | One-operator mutants of the files named, each built and run against the tests; reports the mutants no test notices. |

Parallel examples:

```sh
# Normal suite, local sanitizer suite and source coverage.
ESAJPIP_JOBS=8 ESAJPIP_CTEST_JOBS=8 ./tests/run.sh normal
ESAJPIP_JOBS=8 ESAJPIP_CTEST_JOBS=8 ./tests/run.sh sanitize
ESAJPIP_JOBS=8 ESAJPIP_CTEST_JOBS=8 sh tests/run_baseline.sh
ESAJPIP_JOBS=8 ESAJPIP_CTEST_JOBS=8 \
  ESAJPIP_BASELINE_SPLIT=ON sh tests/run_baseline.sh

# Diagnostic profiles. MSan is for Linux, not the macOS host runtime.
ESAJPIP_JOBS=8 ESAJPIP_CTEST_JOBS=8 sh tests/run_profile.sh asan
ESAJPIP_JOBS=8 ESAJPIP_CTEST_JOBS=8 sh tests/run_profile.sh extended
ESAJPIP_JOBS=8 ESAJPIP_CTEST_JOBS=8 sh tests/run_profile.sh optimized

# Local fuzz campaigns, using Homebrew LLVM on macOS when present.
ESAJPIP_JOBS=8 ESAJPIP_FUZZ_SECONDS=60 ESAJPIP_FUZZ_WORKERS=8 \
  sh tests/run_profile.sh fuzz-extended

# Linux fuzz campaigns in Docker.
ESAJPIP_JOBS=8 ESAJPIP_FUZZ_SECONDS=300 ESAJPIP_FUZZ_WORKERS=8 \
  sh tests/run_linux_docker.sh fuzz-all

# The rules of hv_rules.c that no test sees fail.
ESAJPIP_JOBS=8 ESAJPIP_CTEST_JOBS=8 ESAJPIP_MUTATION_OPERATORS=rule \
  sh tests/run_mutation.sh jpeg2000/hv_rules.c
```

The mutation runner preserves each selected source under `WORK/original`.
Listing IDs, applying mutations and restoring the build copy use that snapshot,
so edits to the checkout during a campaign cannot renumber or change mutants.
An ID identifies a mutation only for the same source bytes and operator set.
Keep the snapshot with the report when investigating survivors. The
`mutation_runner` CTest regression simulates a checkout edit during a run and
checks every built mutant, including the last one. It has the `infrastructure`
label so the default library mutation campaign does not repeat it per mutant.

`tests/jpeg2000/test_rules.c` checks exact rule names, accepted boundary controls,
and observable counts, decoded bytes and state. It covers image/tile origins,
PLT index sets and packet counts, fragment containment and SOC prefixes, URL
percent decoding, palette padding, JPX color parameters, quantization step-list
syntax and restricted ICC tag-directory extents. These complement
the whole-file corpus: executing a rule is insufficient unless tests distinguish
both sides of its boundary. A survivor still needs inspection; for example,
changing a tile-count saturation test from `> 65535` to `>= 65535` leaves the
returned value unchanged at equality and is not a missing validation case.

`tests/fuzz/` holds the fuzz targets of the library and the tools
(`fuzz_reader_rewrite`, `fuzz_deferred_plt`, `fuzz_asn1`, `fuzz_transcode`,
`fuzz_merge`) and `replay`, which includes every target under another name so a
replay mode cannot drift from its target. The targets need Clang's libFuzzer
runtime — Homebrew's LLVM has it, the Apple Command Line Tools do not — so they
are gated by `ESAJPIP_FUZZ=ON` and fail to configure, once, with instructions
when the runtime is missing. CMake generates per-target seed corpora under
`build/.../tests/fuzz/corpus` from the checked-in vectors and fixtures; replay
tests use those corpora in every test build, and the libFuzzer smoke tests use
them when `ESAJPIP_FUZZ=ON`. Seed generation preserves existing files, including
inputs added by fuzz campaigns. Docker campaigns retain their corpora, worker
logs and crash artifacts under `build/linux-diagnostics` (override the parent
directory with `ESAJPIP_LINUX_OUT`), in a new directory for each run.

```sh
build/profile-fuzz/tests/fuzz/fuzz_reader_rewrite \
  build/profile-fuzz/tests/fuzz/corpus/reader-rewrite
build/profile-fuzz/tests/fuzz/fuzz_transcode -jobs=8 -workers=8 \
  build/profile-fuzz/tests/fuzz/corpus/transcode-fuzz
```


## What each test owns

| CTest name | Responsibility |
| --- | --- |
| `model_static` | The model's checks that need no compiler (`spec/check-model.sh --static`). |
| `reader_profile` | The reader against the corpus: every vector the manifest calls standard-valid is accepted, and the profile label is the served profile's. |
| `writer_corpus` | `hv_rewrite` of every vector the reader accepts; byte-comparable rewrites must not change the input. |
| `output_file` | `hv_file`: atomic replacement of the tools' output files. |
| `writer` | The writer with a lowered LBox limit, to reach the switch to XLBox. |
| `served` | `hv_served` with `open`, `fstat`, `read` and `malloc` that misbehave on demand. |
| `reader` | The reader's framing, offsets and errors, byte by byte (`hv_reader.c` included, with allocations that fail on demand), the rules no vector reaches, and the PLT cursor used out of order. |
| `walk_command` | `hv_walk`'s command line: options, exit status and the line it prints for each outcome. |
| `geometry` | `hv_geometry` against T.800 computed a second way. |
| `transcode` | The transcoder's library: packet layout, precincts, PLT, and the profile of its output. |
| `transcode_command` | `hv_transcode`'s command line and its files on disk. |
| `merge` | The merger's library: box construction, JPX graph, color handling. |
| `merge_command` | `hv_merge`'s command line and its files on disk. |
| `logging` | Rotation, truncation, concurrent producers, dropped-record reporting, output failure and shutdown draining. Uses small test-only log limits. |
| `jpip_request` | JPIP parameter parsing, JHV request shapes, selectors, route classification and cache-model diagnostics, linked without the server. |
| `jpip_source` | Byte-range reads and failure boundaries on memory sources. |
| `index_library` | Geometry differential and corpus indexing independent of packet access order. |
| `jpip_packet_layout` | Progression coordinates, resolution selection and compact packet-index bounds. |
| `jpip_cache` | Cache increments, saturation, augmentation and packed complete-bin state. |
| `jpip_window` | Window-to-precinct traversal and resolution boundaries. |
| `jpip_writer` | Exact JPP writer bytes, coalescing, capacity boundaries and metadata placeholders on memory sources. |
| `jpip_library` | Stateful response generation and independent reconstruction of bins across budgets, remapping and repeated windows. |
| `server_support` | Configuration boundaries and missing keys, and mapped-source integration. |
| `server_storage` | Every committed source-vector label, declared packet ranges, linked graphs, malformed files, mapped-source responses and acquisition/release/retry ownership. |
| `server_engine` | Thread migration and exact plain/gzip response equivalence. |
| `server_http` | Split-point and bytewise HTTP parsing, pipeline boundaries, reset and exact limits. |
| `server_overlap` | Real overlapping exchanges, busy admission, waiting/active disconnects, a waiting request that expires, an invalid request for a busy channel, generation failures before/after HTTP headers and shutdown while busy. |
| `server_connection` | Direct connection reports, deadline transitions, response framing, backpressure, failures before/after headers, peer resets and closure. |
| `server` | The real server: HTTP status/headers, admission, limits, descriptor exhaustion, channel routing, disconnects, shutdown and startup failures. Its independent JPP reader reconstructs and verifies source bytes across the full response matrix and stateful scenarios. |
| `fuzz_replay_*` | Deterministic replay of the generated per-target seed corpora through the same assertions used by the libFuzzer targets. |

`reader_profile` and `server_storage` read the same corpus, so a corpus change
concerns both the library and the server. The live JPP checks replace the old
nonempty-body gzip and partial-model smoke tests. Matrix and stateful requests
share one response-draining loop. Configuration cases alter one setting in one
valid INI fixture and identify the setting on failure.

Some overlap is intentional. Writer tests force buffers too small for a header
and check exact encodings. Connection tests inspect report order directly
and script response failures and a client that stops reading. Engine tests
force thread migration.
Those properties are not guaranteed to occur in a live-server run. Keep these
tests rather than replacing them with another successful HTTP request.

## When a test fails

CTest prints the failed test's output. Its complete log is
`<build-directory>/Testing/Temporary/LastTest.log`. Run the matching executable
directly or use `ctest --test-dir <build-directory> -V -R '^test_name$'` for
verbose output. The live-server harness names the failing JPP matrix case and
prints its retained temporary directory for inspecting files and server logs.
Successful live runs remove their temporary files.

Source-vector failures name the file and expected outcome. Follow
[the source-model guide](../spec/README.md#when-a-vector-fails) to investigate
the model, manifest and server together. Do not hand-edit generated vectors or
labels. The [coverage map](../spec/COVERAGE.md) records the tested rules and
intentional boundaries. Regeneration is a separate Docker workflow needed only
when the model or corpus generator changes; it also regenerates
`jpeg2000/generated/`.

## What a passing suite does not establish

These tests are deterministic correctness checks, not a performance benchmark
or a general JPEG 2000 decoder conformance suite. Synthetic packets are not
entropy-decoded. Release qualification still needs Debian 13, a separate
Valgrind run on a non-sanitized build, representative warm/cold workloads, and
a real JHV movie with completion, output and logs checked. Do not run Valgrind
on the sanitizer build or infer production throughput from CTest timings.
