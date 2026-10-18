#!/bin/sh
# Build and run one diagnostic profile of the test suite: the same tests as
# tests/run.sh, under another sanitizer or toolchain. Fuzz and Valgrind are
# explicit because they need toolchain or runtime support that is not on
# every host.
#
#   tests/run_profile.sh asan|extended|msan|optimized
#   tests/run_profile.sh fuzz [CTest options] (needs the libFuzzer runtime)
#   tests/run_profile.sh fuzz-asan|fuzz-extended (bounded libFuzzer campaigns)
#   tests/run_profile.sh valgrind [replay files...]
#
# Builds go to build/profile-MODE, or ESAJPIP_TEST_BUILD_DIR.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$repo/tests/run_common.sh"
mode=${1:-}
shift || true
. "$repo/tests/instrumentation.sh"

usage() {
    echo "Usage: $0 asan|extended|msan|optimized|fuzz|fuzz-asan|fuzz-extended|valgrind [args...]" >&2
}

# The whole suite under one profile. `profile` is an instrumentation.sh name
# (asan additionally passes the build's own ESAJPIP_SANITIZE).
run_ctest_profile() {
    build=$1
    build_type=$2
    profile=$3
    shift 3
    esajpip_instrumentation "$profile"
    # shellcheck disable=SC2086
    cmake -S "$repo" -B "$build" \
        -DBUILD_TESTING=ON \
        -DCMAKE_C_FLAGS="$ESAJPIP_CFLAGS" \
        -DCMAKE_CXX_FLAGS="$ESAJPIP_CXXFLAGS" \
        -DCMAKE_EXE_LINKER_FLAGS="$ESAJPIP_LDFLAGS" \
        $ESAJPIP_CMAKE_EXTRA \
        "$@" \
        -DCMAKE_BUILD_TYPE="$build_type"
    esajpip_build "$build"
    esajpip_ctest "$build" --output-on-failure
}

fuzz_compilers() {
    llvm_prefix=${LLVM_PREFIX:-}
    if [ -z "$llvm_prefix" ] && command -v brew >/dev/null 2>&1; then
        llvm_prefix=$(brew --prefix llvm 2>/dev/null || true)
    fi
    FUZZ_CC=${CC:-clang}
    FUZZ_CXX=${CXX:-clang++}
    if [ -n "$llvm_prefix" ]; then
        FUZZ_CC=$llvm_prefix/bin/clang
        FUZZ_CXX=$llvm_prefix/bin/clang++
    fi
}

fuzz_corpus() {
    case "$1" in
        fuzz_reader_rewrite) echo reader-rewrite ;;
        fuzz_deferred_plt) echo deferred-plt ;;
        fuzz_asn1) echo asn1 ;;
        fuzz_transcode) echo transcode-fuzz ;;
        fuzz_merge) echo merge-fuzz ;;
        fuzz_client_response) echo client-response ;;
        fuzz_client_source) echo client-source ;;
        fuzz_jpip_request) echo jpip-request ;;
        fuzz_jpip_writer) echo jpip-writer ;;
        fuzz_jpip_cache) echo jpip-cache ;;
        fuzz_jpip_session) echo jpip-session ;;
        *)
            echo "unknown fuzz target: $1" >&2
            return 2
            ;;
    esac
}

run_fuzz_campaign() {
    name=$1
    build=$2
    profile=$3
    shift 3
    fuzz_compilers
    esajpip_instrumentation "$profile"
    # shellcheck disable=SC2086
    cmake -S "$repo" -B "$build" \
        -DBUILD_TESTING=ON \
        -DESAJPIP_FUZZ=ON \
        -DCMAKE_C_COMPILER="$FUZZ_CC" \
        -DCMAKE_CXX_COMPILER="$FUZZ_CXX" \
        -DCMAKE_C_FLAGS="$ESAJPIP_CFLAGS" \
        -DCMAKE_CXX_FLAGS="$ESAJPIP_CXXFLAGS" \
        -DCMAKE_EXE_LINKER_FLAGS="$ESAJPIP_LDFLAGS" \
        $ESAJPIP_CMAKE_EXTRA \
        "$@" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo

    build=$(CDPATH= cd -- "$build" && pwd)
    fuzz_targets=${ESAJPIP_FUZZ_TARGETS:-"fuzz_reader_rewrite fuzz_deferred_plt fuzz_asn1 fuzz_transcode fuzz_merge fuzz_client_response fuzz_client_source"}
    # shellcheck disable=SC2086
    esajpip_build "$build" --target $fuzz_targets

    fuzz_seconds=${ESAJPIP_FUZZ_SECONDS:-60}
    fuzz_workers=${ESAJPIP_FUZZ_WORKERS:-8}
    for target in $fuzz_targets; do
        corpus=$(fuzz_corpus "$target")
        run_dir="$build/fuzz-results/$name/$target"
        corpus_dir="$run_dir/corpus"
        artifact_dir="$run_dir/artifacts"
        log="$run_dir/run.log"

        mkdir -p "$artifact_dir" "$corpus_dir"
        cp -R "$build/tests/fuzz/corpus/$corpus/." "$corpus_dir/"
        echo "Running $target under $name for ${fuzz_seconds}s with ${fuzz_workers} workers"
        if ! (cd "$run_dir" && "$build/tests/fuzz/$target" \
            -max_total_time="$fuzz_seconds" \
            -jobs="$fuzz_workers" \
            -workers="$fuzz_workers" \
            -print_final_stats=1 \
            -artifact_prefix="$artifact_dir/" \
            "$corpus_dir") >"$log" 2>&1; then
            cat "$log" >&2
            exit 1
        fi
        grep -E '^(Done |stat::number_of_executed_units|stat::average_exec_per_sec|stat::new_units_added|stat::peak_rss_mb)' "$log" || tail -n 20 "$log"
        artifacts=$(find "$artifact_dir" -type f | wc -l)
        echo "$target artifacts: $artifacts"
    done
}

case "$mode" in
    asan)
        run_ctest_profile "${ESAJPIP_TEST_BUILD_DIR:-$repo/build/profile-asan}" \
            RelWithDebInfo none -DESAJPIP_SANITIZE=ON
        ;;
    extended)
        run_ctest_profile "${ESAJPIP_TEST_BUILD_DIR:-$repo/build/profile-extended}" \
            RelWithDebInfo extended
        ;;
    msan)
        run_ctest_profile "${ESAJPIP_TEST_BUILD_DIR:-$repo/build/profile-msan}" \
            RelWithDebInfo msan
        ;;
    optimized)
        run_ctest_profile "${ESAJPIP_TEST_BUILD_DIR:-$repo/build/profile-optimized}" \
            Release none
        ;;
    fuzz)
        # The fuzz targets need Clang's libFuzzer runtime: Homebrew's LLVM has
        # it, the Apple Command Line Tools do not.
        build=${ESAJPIP_TEST_BUILD_DIR:-$repo/build/profile-fuzz}
        fuzz_compilers
        # LTO off: the fuzz targets carry the coverage runtime.
        esajpip_instrumentation fuzz
        # shellcheck disable=SC2086
        cmake -S "$repo" -B "$build" \
            -DBUILD_TESTING=ON \
            -DESAJPIP_SANITIZE=ON \
            -DESAJPIP_FUZZ=ON \
            -DCMAKE_C_COMPILER="$FUZZ_CC" \
            -DCMAKE_CXX_COMPILER="$FUZZ_CXX" \
            $ESAJPIP_CMAKE_EXTRA \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo
        esajpip_build "$build"
        # A short deterministic run of every target. For a campaign, point a
        # target at a corpus of its own: tests/vectors/j2k and
        # tests/transcode/fixtures are already useful input.
        esajpip_ctest "$build" --output-on-failure -L fuzz "$@"
        ;;
    fuzz-asan)
        run_fuzz_campaign asan "${ESAJPIP_TEST_BUILD_DIR:-$repo/build/profile-fuzz-asan}" \
            fuzz -DESAJPIP_SANITIZE=ON
        ;;
    fuzz-extended)
        run_fuzz_campaign extended "${ESAJPIP_TEST_BUILD_DIR:-$repo/build/profile-fuzz-extended}" \
            extended
        ;;
    valgrind)
        if ! command -v valgrind >/dev/null 2>&1; then
            echo "valgrind is not installed on this host" >&2
            exit 127
        fi
        build=${ESAJPIP_TEST_BUILD_DIR:-$repo/build/profile-valgrind}
        cmake -S "$repo" -B "$build" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
        esajpip_build "$build" --target replay
        if [ "$#" -eq 0 ]; then
            set -- "$repo/tests/vectors/j2k/jp2.jp2" "$repo/tests/vectors/j2k/jp2-precincts.jp2"
        fi
        valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
            --track-origins=yes --error-exitcode=99 \
            "$build/tests/fuzz/replay" reader-rewrite "$@"
        ;;
    *)
        usage
        exit 2
        ;;
esac
