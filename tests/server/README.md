# Server tests

These tests exercise the server adapters and lifecycle. Pure JPIP algorithms
are tested independently under [tests/jpip](../jpip/README.md).

| CTest test | Responsibility |
| --- | --- |
| `server_storage` | FileManager acceptance against the corpus, linked sources, real mapped-source responses, deferred packet errors, stable acquisitions, release, reacquisition and failed-open retry |
| `server_engine` | ChannelEngine thread migration and byte-equivalent plain/gzip output, including one-byte compressed output buffers |
| `server_http` | Every split point and bytewise HTTP parsing, pipelined bytes left unconsumed, parser reset, Host/body rules and exact line/head limits |
| `server_connection` | Direct connection reports with scripted responses: request order, read/write/blocked deadlines, chunk framing, backpressure and its recovery, a final chunk finished by the write callback, failure before/after headers, a peer reset, an interrupted reply, abort and closure |
| `server` | Live HTTP/JPIP routing, source acceptance including an unreadable source, limits, independent bin reconstruction, gzip, cache continuation, a later request without a JPIP route, descriptor exhaustion, a queued image open that times out, shutdown while an image open is blocked, and startup failures |
| `server_overlap` | Live active/waiting/busy exchanges, both disconnect paths, cache preservation, generation failure before/after HTTP headers, an invalid request for a busy channel, a waiting request that expires, and shutdown with a response, a waiting request and an image open pending |
| `server_support`, `logging` | Configuration/storage support and logging behavior |

The former `jpeg2000` server test is now `server_storage`. Its file and response
adapter assertions remain intact. Engine assertions moved into their own
executable. Progression bijection checks moved to `jpip_packet_layout`.
Parser-only assertions moved out of the connection tests into `server_http`.

`channel_fixture.h` copies a checked-in source into a temporary directory for
the channel engine test. `server_fixture.h` shares the live-server and
independent HTTP/JPP decoding harness. The JPP oracle remains independent of
production encoder code and checks exact source bytes, offsets and final flags.

The overlap test uses one source with 8 MiB of opaque metadata to keep a response
active while its client does not drain the socket. It does not model a JHV movie
request pattern or benchmark response generation. Server request logs synchronize
placement of the waiting exchange. No fixed sleep determines which client won
that slot. The shutdown case waits for the stop log before releasing a blocked
open. That server is started with one image-open thread, so the second open
stays queued behind the blocked one.

Three live servers cover the listen address: the channel-limit server listens
on `localhost`, the open-timeout server has no address and listens on every
interface, and one configured with an unresolvable name must not start. That
name is a single 80-character label, which no resolver can put in a query, so
the failure does not wait for a name server.

The descriptor case lowers the descriptor limit to 64 for one server and
connects 100 idle clients. The server cannot accept them all at once; each is
closed by the identification deadline only after it has been accepted, so the
case passes only if the server resumes accepting.

The waiting-timeout case does not depend on how fast the sockets move data.
A waiting request and a response in progress both expire after
`connections.timeout`, two seconds on that server, and the response's wait
restarts whenever it writes. The client leaves the response stalled, sends the
waiting request, and one second later reads 4 MiB of the response, which is
more than the socket buffers hold, so the server must have written again. The
waiting request then expires a second before the response would.

## Run

From the repository root:

```sh
cmake -S . -B build/server-tests -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/server-tests --target server_tests jpip_tests -j8
ctest --test-dir build/server-tests -L '^(server|jpip)$' --output-on-failure -j8
ctest --test-dir build/server-tests -L '^server_channel$' --output-on-failure
ctest --test-dir build/server-tests -R '^server_overlap$' --output-on-failure
ESAJPIP_TEST_TARGETS=server_tests ./tests/run.sh sanitize -L '^server$'
```

Group labels are `server_storage`, `server_channel`, `server_http`,
`server_live` and `server_support`. Every test also has `server`. `server_tests`
builds only these tests and their dependencies. The live tests and direct
connection test share a CTest resource lock. Normal focused tests have a
30-second timeout, and live tests have 90 seconds. The live harness has an
81-second deadline that kills its child server process groups on failure.

These are deterministic regression checks, not exhaustive protocol conformance
or representative JHV performance measurements. Platform-specific qualification
still needs Linux runs and real JHV playback/zoom with overlapping requests.

`-DESAJPIP_TEST_TIMEOUT=seconds` overrides these CTest deadlines. The mutation
runner passes its `ESAJPIP_MUTATION_TIMEOUT` through this setting, so the new
normal-test limits do not override campaign deadlines. The live harness uses
90% of its CTest timeout to clean up children before CTest kills the test.
