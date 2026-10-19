# Fuzz targets and deterministic replay

The existing JPEG 2000, ASN.1, merge and transcode targets are complemented by
four JPIP targets. They link the library directly, without sockets or the
server runtime. Checks live in the harnesses, not in production code.

| Target / replay mode | Input | Independent checks |
| --- | --- | --- |
| `fuzz_jpip_request` / `jpip-request` | Raw request targets plus constructed range/window cases driven by the same bytes | Routing field names, ordered selector union, sampling, model-range endpoints and rejection, frame-size state on failure |
| `fuzz_jpip_writer` / `jpip-writer` | Buffer capacity and a sequence of writes, placeholders and finalizations | Decode JPP headers and payloads, compare accepted contributions, canonical integers, guard bytes and idempotent finalization |
| `fuzz_jpip_cache` / `jpip-cache` | Add, augment, lookup, pack and complete-metadata operations | Compare every stored bin with logical reference lengths, including sparse high precinct IDs and saturation |
| `fuzz_jpip_session` / `jpip-session` | Progression/JP2-or-JPX/padding selectors followed by request histories | Independently constructed packet/header/metadata bytes, final flags, chunk-size independence, byte-budget continuation, truthful cache advertisements, source failure and defined failure outputs |

The session fixture has two resolutions, two components, two layers and a
two-dimensional precinct grid. Embedded JPX contains two codestreams. It covers
all five progression orders. Requests vary rounding, windows, stream/context
selection, quality layers, budgets, source remapping and fresh sessions.
Invalid typed models and zero frame sizes must be rejected. After each accepted
window is completed, small-chunk and large-chunk sessions must agree on their
reconstructed bins. The final full view must equal the independent fixture.
Linked-file graphs and actual HTTP/channel scheduling remain covered by the
index, session and server deterministic tests.

## Run

The ordinary `replay` target builds without libFuzzer. `make_corpus.py` produces
separate seed directories for every target, including boundary cases and
viewport/crop/zoom histories. CTest registers the four JPIP seed replays with
the `jpip_fuzz` label in every test configuration.
CTest gives each corpus a resource lock shared by its replay and fuzzer smoke
tests, preventing concurrent reads while libFuzzer changes corpus files.
Tests using different corpora can run in parallel.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target replay
ctest --test-dir build -L '^jpip_fuzz$' --output-on-failure
build/tests/fuzz/replay jpip-session INPUT_OR_DIRECTORY
```

With a Clang toolchain providing libFuzzer:

```sh
tests/run_profile.sh fuzz -L '^jpip_fuzz$'
tests/run_profile.sh fuzz-extended
```

The profile runner locates Homebrew LLVM when available. In a fuzz build,
`jpip_fuzz` builds only the four JPIP fuzz executables and the common replay
driver. Both `jpip` and `jpeg2000` receive coverage-guided instrumentation.
Each smoke test uses a fixed seed and 20,000 executions, with a 60-second CTest
timeout. The request target also uses `jpip.dict`. The `fuzz-asan` and
`fuzz-extended` profile modes run bounded mutation campaigns and keep corpora,
logs and artifacts under the build tree.

For a longer campaign, copy a target's generated seed directory into a separate
corpus directory and run its executable, for example:

```sh
build/profile-fuzz/tests/fuzz/fuzz_jpip_session CORPUS \
    -max_total_time=60 -max_len=4096 -timeout=5 -rss_limit_mb=512
```

Replay that campaign corpus through the ordinary driver under the sanitizer,
coverage or Valgrind profile. `ESAJPIP_REPLAY_VERBOSE=1` names each input before
running it. Keep failing inputs and minimize them before adding regression
cases. Decoder checks do not use the production writer as an expected-output
generator.

## Input contracts

All targets bound operation counts and allocations. Direct API calls respect
their preconditions. Arbitrary malformed request text is still passed to the
parser. Cache metadata IDs and stream counts are bounded; high IDs are used
for sparse precincts. Session files stay valid so mutations reach response
generation instead of repeatedly failing the file parser.

- Writer: the first byte selects a capacity. Bit 7 instead selects a following
  two-byte capacity modulo 33000. Each write starts with flags, class, stream,
  bin and offset boundary selectors, payload-length selector and source offset.
  Low three flag bits zero finalize the buffer. Other flags select adjacent
  writes, final contributions or placeholder slices. Boundary selectors with
  bit 7 set consume a following eight-byte integer. Placeholder controls add
  original ID, kind, header size and skip.
- Cache: six bytes select operation, class, stream, ID, amount and completion.
  A precinct ID selector with bit 7 set selects IDs near `INT_MAX`. Packing is
  absent from the reference model because it must not change logical contents.
- Session: three bytes select progression, JP2/JPX and trailing PLT padding.
  Each following eleven-byte record controls mode, selectors, frame width and
  height, window position and size, layers, budget and chunk capacity. Mode bits
  enable windows, cropping, invalid models, fresh sessions, source remapping
  and invalid frame sizes. Cache advertisements report only bytes actually
  received. A byte-limited attempt is followed by a request completing the same
  window, including after an EOR-only response.

Use production-file line/branch coverage to measure reach. Fuzzer counters also
include harnesses and standard-library code. High executed-line coverage alone
does not establish that semantic checks would catch an incorrect result.

## Client

Two targets cover the C client shared by native callers and WASM:

| Target / replay mode | Input | Checks |
| --- | --- | --- |
| `fuzz_client_response` / `client-response` | Raw JPP response body | Parser/cache acceptance versus source ingestion, EOR, metadata/XML/palette agreement, bounded status and reconstruction agreement |
| `fuzz_client_source` / `client-source` | Four selector bytes followed by a response body | Prepared request completion, quality preservation during metadata replay, stable model batches, reconstruction guard bytes and repeatability, decoder dimensions |

`client_fuzz_seeds` generates valid responses using `DataBinServer`, from a
small RGB image, a gray image and the eight-frame mixed-size JPX fixture.
It seeds lowest/full resolution and preview/full quality, plus truncated
responses. These are valid starting points, not independent expected-output
oracles. Pixel correctness remains checked by the native reconstruction tests.

The source target's first four bytes select fixture, frame, reduction and
layers. Fixture selection is modulo three, frame selection modulo its frame
count; reduction 255 selects the lowest level and layers zero selects full
quality. Each iteration starts with that immutable fixture's complete headers
and metadata, then applies the mutated response to a prepared frame request.
This reaches precinct ingestion, quality confirmation, reconstruction and
OpenJPEG without requiring mutations to preserve a valid main header first.
It also exercises model batching and exact metadata replay, including after
failed ingestion. Fixtures are loaded once per process.

Both targets reject inputs larger than 1 MiB. The raw-response target parses
arbitrary headers but only traverses geometry with at most 4,096 precincts,
1,048,576 pixels, four components and 32 layers, and inspects at most 16 frames.
Reconstruction buffers are capped at 2 MiB. The source target uses fixed fixture
geometry and decodes only selected resolutions with at most 1,048,576 pixels
and four components. Larger frames still exercise cache and reconstruction.
These are harness work bounds, not production limits.

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target replay
ctest --test-dir build -L '^client_fuzz$' --output-on-failure
build/tests/fuzz/replay client-source INPUT_OR_DIRECTORY

# With Homebrew LLVM (or another Clang with libFuzzer):
ESAJPIP_FUZZ_TARGETS='fuzz_client_response fuzz_client_source' \
ESAJPIP_FUZZ_SECONDS=30 ESAJPIP_FUZZ_WORKERS=1 \
    sh tests/run_profile.sh fuzz-asan
```

In a fuzz build, `client_fuzz` builds both executables and replay. The C client
and OpenJPEG receive coverage-guided instrumentation, as do the JPEG 2000 and
JPIP libraries. Each client CTest smoke run uses 1,000 executions, a fixed seed,
a 256 KiB maximum input size and a 60-second timeout. Campaigns copy the seed
corpus and retain discoveries separately from the generated seeds.
