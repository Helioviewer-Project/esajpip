# JPIP library tests

Every executable here links `jpip` and its `jpeg2000` dependency. No test uses
server headers, file-mapping adapters, sockets, HTTP, gzip or server logging.
The index/session tests load checked-in fixtures into memory. The other tests
construct their inputs directly.

| Directory | CTest tests | Owns |
| --- | --- | --- |
| `source/` | `jpip_source` | Absolute reads, end boundaries and unchanged destinations on failed reads |
| `index/` | `index_library`, `jpip_packet_layout` | Corpus indexing, deferred errors, access order, progression coordinates, resolution selection and compact-index bounds |
| `request/` | `jpip_request` | JPIP syntax, selectors, cache-model descriptors and routing-field values |
| `response/` | `jpip_cache`, `jpip_window`, `jpip_writer` | Sparse cache packing, independent 2D packet sequences, canonical JPP integer boundaries, coalescing, buffer limits and placeholders |
| `response/` | `jpip_library` | Stateful responses: independently reconstructed bins, source failures, remapping, budgets, repeated requests, viewport-to-zoom reuse, layer limits and interleaved independent sessions |

`tests/jpeg2000/test_geometry.c` checks the C format library against independent
standard-derived geometry. `index/test_packet_layout.cc` checks the C++
`CodingParameters` and compact `PacketIndex` contracts used for serving.
`index/test_index.cc` also compares the two implementations and checks that
packet access order does not change indexed ranges or deferred errors.

The session tests deliberately combine layers of the library. They complement
the focused tests. File mapping, HTTP extraction, channel lifecycle, gzip and
connection handling remain integration tests under `tests/server/`.

## Build and select

From the repository root:

```sh
cmake -S . -B build/jpip-tests -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/jpip-tests --target jpip_tests -j8
ctest --test-dir build/jpip-tests -L '^jpip$' --output-on-failure
ctest --test-dir build/jpip-tests -L '^jpip_response$' --output-on-failure
ctest --test-dir build/jpip-tests -R '^jpip_writer$' --output-on-failure
```

Every library test has a 30-second CTest timeout.

`jpip_tests` builds only these tests and their library dependencies. The project
configuration still discovers the server's development packages. Group labels
are `jpip_source`, `jpip_index`, `jpip_request` and `jpip_response`; every test
also keeps the `tools` label used by the existing runners.

The same suite supports ASan/UBSan via `-DESAJPIP_SANITIZE=ON` in a separate
build, or through the common runner:

```sh
ESAJPIP_TEST_TARGETS=jpip_tests ./tests/run.sh sanitize -L '^jpip$'
```

## Add a test

Place focused tests beside the responsibility they exercise. Add one entry to
`tests/jpip/CMakeLists.txt`, for example:

```cmake
jpip_add_test(test_jpip_selection jpip_selection request request/test_selection.cc)
```

This registers the executable, compiler/sanitizer settings, library link,
CTest labels and `jpip_tests` dependency. Add fixture definitions only when the
test reads the corpus. There is no test framework dependency or server fixture
required to add a library case.

Use explicit expected bytes, coordinates and state transitions. Preserve the
independent JPP decoder in the session tests; using the production writer as
the expected-result generator would hide matching errors. Keep malformed inputs
and failure diagnostics when extending coverage. Introduce shared fixtures
when multiple test files actually need them.

The suite also checks sparse complete prefixes separated by holes, regrowing
packed cache storage, exact LRCP packet sequences from independently enumerated
2D windows, and Bin-ID/VBAS boundaries through `UINT64_MAX`. Independent sessions
use different budgets while their responses are outstanding. A third session's
source failure must not change either response or its cache. Borrowed sources
remain alive through each response; remapping is tested between responses, as
required by the provider contract.

## Fuzz coverage

The request, writer, cache and session fuzz targets link this library directly.
They share the project's deterministic replay driver and have independent
semantic checks. See [fuzz targets and replay](../fuzz/README.md) for input
contracts and campaigns.

```sh
cmake --build build --target replay
ctest --test-dir build -L '^jpip_fuzz$' --output-on-failure
```

`tests/run_baseline.sh` with `ESAJPIP_BASELINE_SPLIT=ON` also writes separate
`jpip` and `jpip-fuzz` reports, so replay coverage can be compared with the
ordinary library tests.
