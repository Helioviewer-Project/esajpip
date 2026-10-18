#!/bin/sh
# Build and run the whole test suite with Clang source coverage, then report
# it in three parts: the hand-written code, the generated model code, and the
# generated ASN.1 runtime. tests/COVERAGE_GAPS.md reads these reports.
#
#   tests/run_baseline.sh
#
# Environment: ESAJPIP_BASELINE_BUILD, ESAJPIP_BASELINE_OUT, LLVM_COV,
# LLVM_PROFDATA, TIME_CMD, LLVM_PREFIX (Homebrew LLVM, as in run_profile.sh),
# ESAJPIP_JOBS, ESAJPIP_CTEST_JOBS, ESAJPIP_BASELINE_SPLIT=ON to also report
# coverage by test group.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$repo/tests/run_common.sh"
. "$repo/tests/instrumentation.sh"
build=${ESAJPIP_BASELINE_BUILD:-$repo/build/coverage}
out=${ESAJPIP_BASELINE_OUT:-$repo/build/coverage-report}

# The LLVM tools may come from Homebrew's LLVM, which is where the sanitizer
# and fuzz runtimes come from too.
llvm_prefix=${LLVM_PREFIX:-}
if [ -z "$llvm_prefix" ] && command -v brew >/dev/null 2>&1; then
    llvm_prefix=$(brew --prefix llvm 2>/dev/null || true)
fi
llvm_cov=${LLVM_COV:-}
llvm_profdata=${LLVM_PROFDATA:-}
if [ -z "$llvm_cov" ]; then
    if [ -n "$llvm_prefix" ] && [ -x "$llvm_prefix/bin/llvm-cov" ]; then
        llvm_cov=$llvm_prefix/bin/llvm-cov
    else
        llvm_cov=$(command -v llvm-cov || xcrun --find llvm-cov)
    fi
fi
if [ -z "$llvm_profdata" ]; then
    if [ -n "$llvm_prefix" ] && [ -x "$llvm_prefix/bin/llvm-profdata" ]; then
        llvm_profdata=$llvm_prefix/bin/llvm-profdata
    else
        llvm_profdata=$(command -v llvm-profdata || xcrun --find llvm-profdata)
    fi
fi
time_cmd=${TIME_CMD:-/usr/bin/time}

rm -rf "$build" "$out"
mkdir -p "$out/profiles"

# The profiler and the compiler must be the same LLVM, so when Homebrew's
# LLVM provided llvm-cov it also provides clang.
compilers=
if [ -n "$llvm_prefix" ] && [ -x "$llvm_prefix/bin/clang" ]; then
    compilers="-DCMAKE_C_COMPILER=$llvm_prefix/bin/clang -DCMAKE_CXX_COMPILER=$llvm_prefix/bin/clang++"
fi

esajpip_instrumentation coverage

# shellcheck disable=SC2086
cmake -S "$repo" -B "$build" \
    -DBUILD_TESTING=ON \
    -DCMAKE_C_FLAGS="$ESAJPIP_CFLAGS" \
    -DCMAKE_CXX_FLAGS="$ESAJPIP_CXXFLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="$ESAJPIP_LDFLAGS" \
    $ESAJPIP_CMAKE_EXTRA \
    $compilers \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo

esajpip_build "$build"

timed_ctest() {
    build_dir=$1
    shift
    if [ -n "$esajpip_ctest_jobs" ]; then
        "$time_cmd" -l ctest --test-dir "$build_dir" -j "$esajpip_ctest_jobs" "$@"
    else
        "$time_cmd" -l ctest --test-dir "$build_dir" "$@"
    fi
}

ctest_status=0
LLVM_PROFILE_FILE="$out/profiles/%p.profraw"
export LLVM_PROFILE_FILE
timed_ctest "$build" --output-on-failure \
    > "$out/ctest.log" 2> "$out/time.log" || ctest_status=$?
unset LLVM_PROFILE_FILE

objects=$(find "$build" -type f -perm -111 \
    ! -name '*.dSYM' ! -name '*.a' ! -name '*.dylib' | sort)
first=$(printf '%s\n' "$objects" | sed -n '1p')
rest=$(printf '%s\n' "$objects" | sed '1d' | sed 's/^/-object=/' | tr '\n' ' ')

report() {
    profile=$1
    output=$2
    shift 2
    # shellcheck disable=SC2086
    "$llvm_cov" report "$first" $rest -instr-profile="$profile" "$@" \
        > "$output"
}

write_reports() {
    dir=$1
    profile=$2
    mkdir -p "$dir"
    report "$profile" "$dir/handwritten.txt" \
        "$repo/jpeg2000" "$repo/jpip" "$repo/merge" "$repo/transcode" "$repo/server" "$repo/tools" \
        -ignore-filename-regex='/jpeg2000/generated/|/server/vendor/|/tests/' || return $?
    report "$profile" "$dir/generated.txt" \
        "$repo/jpeg2000/generated" \
        -ignore-filename-regex='/jpeg2000/generated/asn1crt' || return $?
    report "$profile" "$dir/asn1-runtime.txt" \
        "$repo/jpeg2000/generated/asn1crt.c" \
        "$repo/jpeg2000/generated/asn1crt_encoding.c" \
        "$repo/jpeg2000/generated/asn1crt_encoding_acn.c" \
        "$repo/jpeg2000/generated/asn1crt_encoding_uper.c"
}

"$llvm_profdata" merge -sparse "$out"/profiles/*.profraw -o "$out/coverage.profdata"
write_reports "$out" "$out/coverage.profdata"

split_status=0
split_reports=

run_split() {
    name=$1
    shift
    dir="$out/by-test/$name"
    mkdir -p "$dir/profiles"
    status=0
    LLVM_PROFILE_FILE="$dir/profiles/%p.profraw"
    export LLVM_PROFILE_FILE
    timed_ctest "$build" "$@" --output-on-failure \
        > "$dir/ctest.log" 2> "$dir/time.log" || status=$?
    unset LLVM_PROFILE_FILE
    if ls "$dir"/profiles/*.profraw >/dev/null 2>&1; then
        "$llvm_profdata" merge -sparse "$dir"/profiles/*.profraw \
            -o "$dir/coverage.profdata" || return $?
        write_reports "$dir" "$dir/coverage.profdata" || return $?
    else
        cat > "$dir/no-coverage.txt" <<EOF
No instrumented C or C++ binaries ran for this group.
EOF
    fi
    return "$status"
}

if [ "${ESAJPIP_BASELINE_SPLIT:-OFF}" = ON ]; then
    split_reports="$out/by-test"
    if [ "$ctest_status" -eq 0 ]; then
        run_split normal-no-replay -E '^fuzz_replay_' || split_status=$?
        run_split jpeg2000 -R '^(reader_profile|writer_corpus|output_file|writer|served|reader|geometry)$' || split_status=$?
        run_split jpip -L '^jpip$' || split_status=$?
        run_split jpip-fuzz -L '^jpip_fuzz$' || split_status=$?
        run_split merge -R '^(merge|merge_command)$' || split_status=$?
        run_split transcode -R '^(transcode|transcode_command)$' || split_status=$?
        run_split server -L '^server$' || split_status=$?
        run_split replay -R '^fuzz_replay_' || split_status=$?
        run_split jpeg2000-merge-transcode -R '^(reader_profile|writer_corpus|output_file|writer|served|reader|geometry|merge|merge_command|transcode|transcode_command)$' || split_status=$?
    else
        mkdir -p "$split_reports"
        cat > "$split_reports/skipped.txt" <<EOF
Split coverage was skipped because the full CTest run failed.
EOF
    fi
fi

cat > "$out/summary.txt" <<EOF
build=$build
output=$out
coverage_profile=$out/coverage.profdata
test_log=$out/ctest.log
time_log=$out/time.log
ctest_status=$ctest_status
split_status=$split_status
split_reports=$split_reports
reports:
  $out/handwritten.txt
  $out/generated.txt
  $out/asn1-runtime.txt
EOF

cat "$out/summary.txt"
if [ "$ctest_status" -ne 0 ]; then
    exit "$ctest_status"
fi
exit "$split_status"
