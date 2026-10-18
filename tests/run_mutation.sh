#!/bin/sh
# Mutation testing: one-operator mutants of the files named (tests/mutate.py),
# each built and run against the tests. A mutant the tests pass survives: a
# change of behaviour that no test notices.
#
#   tests/run_mutation.sh [FILE...]     default: jpeg2000/hv_rules.c jpeg2000/hv_reader.c
#
# FILE is relative to the repository. The sources are copied to WORK/src,
# which is built in WORK/build, so the checkout is never modified. Each
# mutant is written into the copy, built (incrementally), run, and the file
# put back. Listing, applying and restoring all use WORK/original, saved
# from that same copy; editing the checkout cannot change the mutant IDs.
# Before the first mutant the unmutated copy must pass.
#
# Environment:
#   ESAJPIP_TEST_BUILD_DIR      WORK (default build/mutation)
#   ESAJPIP_MUTATION_OPERATORS  mutate.py operators (default
#                               rule,relational,logical,negation)
#   ESAJPIP_MUTATION_LINES      FIRST-LAST: only the mutants on those lines
#   ESAJPIP_MUTATION_EVERY      N: every Nth mutant only (default 1)
#   ESAJPIP_MUTATION_TESTS      the CTest selection (default "-L tools")
#   ESAJPIP_MUTATION_TIMEOUT    seconds per test (default 120)
#   ESAJPIP_JOBS, ESAJPIP_CTEST_JOBS, as for the other runners
#
# WORK/report.tsv has a line per mutant: file, mutant, line, operator,
# change, and killed, survived, timeout (a test ran out of time) or build
# (it does not compile). WORK/survivors.txt lists the survivors.
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$repo/tests/run_common.sh"
work=${ESAJPIP_TEST_BUILD_DIR:-$repo/build/mutation}
operators=${ESAJPIP_MUTATION_OPERATORS:-rule,relational,logical,negation}
lines=${ESAJPIP_MUTATION_LINES:-}
every=${ESAJPIP_MUTATION_EVERY:-1}
tests=${ESAJPIP_MUTATION_TESTS:--L tools}
timeout=${ESAJPIP_MUTATION_TIMEOUT:-120}
python=${PYTHON:-python3}
[ $# -gt 0 ] || set -- jpeg2000/hv_rules.c jpeg2000/hv_reader.c

first=
last=
if [ -n "$lines" ]; then
    first=${lines%-*}
    last=${lines#*-}
fi

src=$work/src
original=$work/original
build=$work/build
report=$work/report.tsv
survivors=$work/survivors.txt

# The copy: everything but build/ and .git, refreshed for each run.
rm -rf "$src"
mkdir -p "$src" "$build"
for entry in "$repo"/* "$repo"/.[!.]*; do
    [ -e "$entry" ] || continue
    case ${entry##*/} in
        build|.git) continue ;;
    esac
    cp -R "$entry" "$src/"
done
for file in "$@"; do
    [ -f "$src/$file" ] || { echo "run_mutation.sh: no $file" >&2; exit 2; }
    mkdir -p "$original/$(dirname "$file")"
    cp "$src/$file" "$original/$file"
done

cmake -S "$src" -B "$build" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
    -DESAJPIP_TEST_TIMEOUT="$timeout" >"$work/configure.log"

# ctest's status, and whether a test timed out, for the current copy.
run_tests() {
    set +e
    # shellcheck disable=SC2086
    esajpip_ctest "$build" $tests --no-tests=error --timeout "$timeout" >"$work/ctest.log" 2>&1
    status=$?
    set -e
    if [ "$status" -ne 0 ] && grep -q 'Timeout' "$work/ctest.log"; then
        return 2
    fi
    return "$status"
}

echo "unmutated: building and testing"
esajpip_build "$build" >"$work/build.log" 2>&1 ||
    { echo "run_mutation.sh: the unmutated sources do not build (see $work/build.log)" >&2; exit 1; }
run_tests || { echo "run_mutation.sh: the unmutated sources fail the tests (see $work/ctest.log)" >&2; exit 1; }

: >"$report"
killed=0
survived=0
timeouts=0
broken=0
for file in "$@"; do
    "$python" "$src/tests/mutate.py" list "$original/$file" "$operators" >"$work/mutants.tsv"
    total=$(wc -l <"$work/mutants.tsv" | tr -d ' ')
    while IFS='	' read -r id line operator change; do
        [ $((id % every)) -eq 0 ] || continue
        if [ -n "$first" ] && { [ "$line" -lt "$first" ] || [ "$line" -gt "$last" ]; }; then
            continue
        fi
        "$python" "$src/tests/mutate.py" apply "$original/$file" "$id" "$src/$file" "$operators"
        if ! esajpip_build "$build" >"$work/build.log" 2>&1; then
            outcome=build
            broken=$((broken + 1))
        elif run_tests; then
            outcome=survived
            survived=$((survived + 1))
        elif [ $? -eq 2 ]; then
            outcome=timeout
            timeouts=$((timeouts + 1))
        else
            outcome=killed
            killed=$((killed + 1))
        fi
        cp "$original/$file" "$src/$file"
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$file" "$id" "$line" "$operator" "$change" "$outcome" \
            >>"$report"
        echo "$file mutant $id/$total, line $line, $operator $change: $outcome"
    done <"$work/mutants.tsv"
done
# The last mutant's objects are rebuilt from the restored sources next run.
esajpip_build "$build" >"$work/build.log" 2>&1 || true

awk -F '\t' '$6 == "survived" { printf "%s:%s: %s %s\n", $1, $3, $4, $5 }' "$report" >"$survivors"
echo "killed $killed, survived $survived, timeout $timeouts, not built $broken"
echo "report: $report"
echo "survivors: $survivors"
