# Command support

`hv_file.c` and `hv_file.h` provide atomic output replacement for `hv_merge`
and `hv_transcode`. CMake compiles them directly into each command and the
output-file test. They are not part of `jpeg2000`. Output is flushed and
closed before rename; crash durability is not guaranteed. A shared 128 KiB
stdio buffer batches output writes for the single active output.

This helper owns process-wide signal cleanup and allows one active output.
It also briefly changes the process umask when reading it. These policies
belong to the commands; library callers supply their own output and lifecycle.

`hv_cli.c` and `hv_cli.h` parse the `-p W,H` value of `hv_transcode` and
`hv_merge --transcode` (`hv_cli_precincts`): W and H are powers of 2 from 2
to 32,768, written as decimal digits only, with no sign, space or other
character. The result is a pair of exponents from 1 to 15. Like `hv_file`,
it is compiled into each command and is not part of the transcoder library.

## JPIP benchmark

`jpip_bench.cc` measures target opening and a persistent response sequence with
no network or gzip: frames 0–4 at 1024 resolution, then frame 0 at 4096. Use a
linked movie containing at least five frames. `open` averages ten openings per
process (an optional final argument changes that count). `response` reports
opening, each response's time/byte count/FNV-1a hash, and process peak RSS.
Times are milliseconds, RSS is bytes; response timings include hashing.

```sh
cmake -S . -B build/benchmark -DCMAKE_BUILD_TYPE=Release
cmake --build build/benchmark --target jpip_bench -j8
build/benchmark/jpip_bench open /path/to/images movie-linked.jpx
build/benchmark/jpip_bench response /path/to/images movie-linked.jpx
```

`benchmark-jpip.py` alternates execution order between two binaries, performs
one warmup and seven measured runs by default, and emits raw samples plus
medians as JSON. Opening and response modes run in separate processes. The
runner reports wire equality by phase; a difference needs investigation,
since changed fragmentation can preserve the reconstructed bins.

```sh
python3 tools/benchmark-jpip.py /path/to/before/jpip_bench \
  build/benchmark/jpip_bench /path/to/images movie-linked.jpx \
  --runs 7 > benchmark.json
```

Use the same source, compiler, optimization/LTO configuration and fixtures for
both binaries. For pre-migration `84100e0`, which has no benchmark target, build
its `esajpip_core` first and compile the benchmark from `b2a56d7` against
that archive. It uses the former header paths and the same workload. On the
macOS Clang/ThinLTO configuration used for the integration measurements:

```sh
# OLD_SRC and OLD_BUILD identify the separate baseline checkout and build.
cmake -S "$OLD_SRC" -B "$OLD_BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$OLD_BUILD" --target esajpip_core -j8
git show b2a56d7:tools/jpip_bench.cc > "$OLD_BUILD/jpip_bench.cc"
c++ -std=c++11 -O3 -DNDEBUG -flto=thin -I"$OLD_SRC/src" -I"$OLD_SRC/src/server" \
  "$OLD_BUILD/jpip_bench.cc" "$OLD_BUILD/libesajpip_core.a" \
  $(pkg-config --cflags --libs glib-2.0 libllhttp libuv) -lz \
  -o "$OLD_BUILD/jpip_bench"
```

Select the matching compiler/LTO flags on other platforms. RSS is a process
high-water mark, not the size of the library's allocations. Warm-cache timings
are not disk-cold opening measurements or a claim about other hosts.
