#!/bin/sh
# The hv_merge command: plain options, exit status, and nothing
# written or left behind on failure.
#   cli_test.sh HV_MERGE HV_TRANSCODE MERGE_FIXTURES TRANSCODE_FIXTURES WORK
set -eu
exe=$1
transcode=$2
fixtures=$3
transcode_fixtures=$4
kakadu=$transcode_fixtures/kakadu
work=$5
rm -rf "$work"
mkdir -p "$work"
expected="$fixtures/expected/merged.jpx"
failures=0

fail() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

status() {
    set +e
    "$@" >"$work/stdout" 2>"$work/stderr"
    code=$?
    set -e
    echo $code
}

rest="$kakadu/2015_12_21__00_10_34_34__SDO_AIA_AIA_171.jp2 \
$kakadu/solo_fsi174_127x129_RLCP_PLT.jp2 \
$kakadu/solo_fsi174_509x513_PCRL.jp2 $kakadu/solo_fsi174_510x514_LRCP.jp2 \
$kakadu/solo_fsi174_511x513_LRCP_PLT.jp2 $kakadu/synthetic_rgb_129x129_CPRL_SOP_EPH.jp2"

[ "$(status "$exe")" = 2 ] || fail "no arguments: expected exit status 2"
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2")" = 2 ] || fail "no -o: expected 2"
[ "$(status "$exe" -o "$work/out.jpx")" = 2 ] || fail "no -i: expected 2"
[ "$(status "$exe" -q -i "$fixtures/input/swap_000.jp2" -o "$work/out.jpx")" = 2 ] ||
    fail "unknown option: expected 2"
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -x.jp2 -o "$work/out.jpx")" = 2 ] ||
    fail "a name starting with - after -i: expected 2"
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -o "$work/out.jpx" --)" = 2 ] ||
    fail "--: expected 2"
for option in --trans -lin -hi -oattached.jpx -o=equals.jpx --transcode=yes; do
    [ "$(status "$exe" "$option" -i "$fixtures/input/swap_000.jp2" -o "$work/out.jpx")" = 2 ] ||
        fail "non-exact option $option: expected 2"
done
for option in -i -o -p -s; do
    [ "$(status "$exe" "$option")" = 2 ] || fail "$option without value: expected 2"
    [ "$(status "$exe" "$option" --validate)" = 2 ] || fail "$option before an option: expected 2"
done
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -o "$work/out.jpx" -o "$work/other.jpx")" = 2 ] ||
    fail "duplicate -o: expected 2"
[ ! -e "$work/out.jpx" ] && [ ! -e "$work/other.jpx" ] || fail "a usage error wrote the output"
for option in -h --help; do
    [ "$(status "$exe" "$option")" = 0 ] && grep -q "^usage: hv_merge" "$work/stdout" &&
        grep -q -- "-s FILE" "$work/stdout" ||
        fail "$option: expected the usage on standard output and exit status 0"
    # Help comes before any argument file is read, even a missing one.
    [ "$(status "$exe" "$option" -s "$work/missing-args")" = 0 ] ||
        fail "$option -s missing: expected help"
    [ "$(status "$exe" -s "$work/missing-args" "$option")" = 0 ] ||
        fail "-s missing $option: expected help"
done
# "-" alone is never a value of -i or -o.
[ "$(status "$exe" -i - -o "$work/out.jpx")" = 2 ] || fail "-i -: expected 2"
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -o -)" = 2 ] || fail "-o -: expected 2"
# Help wins over an unknown option, and also comes from an argument file.
[ "$(status "$exe" -q -h)" = 0 ] && grep -q "^usage: hv_merge" "$work/stdout" ||
    fail "-q -h: expected help"
printf -- '-h\n' >"$work/help-args"
[ "$(status "$exe" -s "$work/help-args")" = 0 ] && grep -q "^usage: hv_merge" "$work/stdout" ||
    fail "-h in an argument file: expected help"
# No names left once empty names and commas are dropped.
[ "$(status "$exe" -i '' -o "$work/out.jpx")" = 1 ] && grep -q "no JP2 input files" "$work/stderr" ||
    fail "-i '': expected \"no JP2 input files\""
[ "$(status "$exe" -i , -o "$work/out.jpx")" = 1 ] && grep -q "no JP2 input files" "$work/stderr" ||
    fail "only commas: expected 1 and \"no JP2 input files\""
[ ! -e "$work/out.jpx" ] || fail "a failed merge wrote the output"
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -o "")" = 1 ] || fail "-o \"\": expected 1"

# Comma and space separated names, stray commas included.
# shellcheck disable=SC2086
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2,,$fixtures/input/swap_001.jp2," $rest \
    -o "$work/out.jpx")" = 0 ] || fail "merge failed: $(cat "$work/stderr")"
cmp -s "$work/out.jpx" "$expected" || fail "the output differs from hvJP2K's"

# Repeated -i accumulates names in order.
# shellcheck disable=SC2086
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -i "$fixtures/input/swap_001.jp2" \
    -i $rest -o "$work/repeated.jpx")" = 0 ] || fail "repeated -i: $(cat "$work/stderr")"
cmp -s "$work/repeated.jpx" "$expected" || fail "repeated -i gives other bytes"

# File arguments are inserted at -s, with space, tab, newline and carriage
# return separating words, including repeated and trailing whitespace.
{
    printf ' \t\r\n-i\t%s\r%s' "$fixtures/input/swap_000.jp2" "$fixtures/input/swap_001.jp2"
    for f in $rest; do printf '\n%s' "$f"; done
    printf '\n-o %s \t\r\n' "$work/argfile.jpx"
} >"$work/args"
[ "$(status "$exe" -s "$work/args")" = 0 ] || fail "-s failed: $(cat "$work/stderr")"
cmp -s "$work/argfile.jpx" "$expected" || fail "-s gives other bytes"
# Bare file words continue an active -i at this position, before the next
# command-line input and -o.
printf '%s\n' "$fixtures/input/swap_001.jp2" >"$work/names"
# shellcheck disable=SC2086
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -s "$work/names" $rest \
    -o "$work/position.jpx")" = 0 ] || fail "-s position: $(cat "$work/stderr")"
cmp -s "$work/position.jpx" "$expected" || fail "-s position gives other bytes"
printf -- '-i %s\n' "$fixtures/input/swap_001.jp2" >"$work/more-inputs"
# shellcheck disable=SC2086
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -s "$work/more-inputs" $rest \
    -o "$work/more.jpx")" = 0 ] || fail "-i in argument file: $(cat "$work/stderr")"
cmp -s "$work/more.jpx" "$expected" || fail "-i in argument file gives other bytes"
# Two argument files: inputs from the first, the rest from the second.
{
    printf -- '-i %s' "$fixtures/input/swap_000.jp2,$fixtures/input/swap_001.jp2"
    for f in $rest; do printf ' %s' "$f"; done
    printf '\n'
} >"$work/first-args"
printf -- '-o %s\n' "$work/two.jpx" >"$work/second-args"
[ "$(status "$exe" -s "$work/first-args" -s "$work/second-args")" = 0 ] ||
    fail "two argument files: $(cat "$work/stderr")"
cmp -s "$work/two.jpx" "$expected" || fail "two argument files give other bytes"
printf -- '-s %s\n' "$work/missing-args" >"$work/nested-args"
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -s "$work/nested-args" \
    -o "$work/nested.jpx")" = 2 ] || fail "nested -s: expected 2"
[ ! -e "$work/nested.jpx" ] || fail "nested -s wrote the output"
[ "$(status "$exe" -o "$work/duplicate.jpx" -s "$work/args")" = 2 ] || fail "duplicate -o in argument file: expected 2"
[ ! -e "$work/duplicate.jpx" ] || fail "duplicate -o wrote the output"
printf -- '-i %s -o %s -links\f\n' "$fixtures/input/swap_000.jp2" "$work/ff.jpx" >"$work/ff-args"
[ "$(status "$exe" -s "$work/ff-args")" = 2 ] || fail "a form feed in the argument file: expected 2"
[ ! -e "$work/ff.jpx" ] || fail "a form feed in the argument file: the output was written"

# An argument file read only in part must not merge the part: a read
# error (a directory) and a NUL byte (which would end the text) fail.
mkdir "$work/argdir"
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -o "$work/partial.jpx" \
    -s "$work/argdir")" = 1 ] ||
    fail "-s with a read error: expected 1"
printf -- '-i %s\0%s\n' "$fixtures/input/swap_000.jp2" "$fixtures/input/swap_001.jp2" \
    >"$work/nul-args"
[ "$(status "$exe" -o "$work/partial.jpx" -s "$work/nul-args")" = 1 ] ||
    fail "-s with a NUL byte: expected 1"
[ ! -e "$work/partial.jpx" ] || fail "a partly read -s file gave a merge"
rmdir "$work/argdir"

# A single-quoted name with spaces in the argument file.
cp "$fixtures/input/swap_000.jp2" "$work/single quoted.jp2"
{
    printf -- "-i '%s' %s" "$work/single quoted.jp2" "$fixtures/input/swap_001.jp2"
    for f in $rest; do printf ' %s' "$f"; done
    printf '\n-o %s\n' "$work/single.jpx"
} >"$work/single-args"
[ "$(status "$exe" -s "$work/single-args")" = 0 ] ||
    fail "single quotes in the argument file: $(cat "$work/stderr")"
cmp -s "$work/single.jpx" "$expected" || fail "single quotes in the argument file: other bytes"
rm -f "$work/single quoted.jp2" "$work/single.jpx" "$work/single-args"
# Quoting in the argument file: a backslash outside quotes, a quote escaped
# inside double quotes, runs of blanks and blanks at the ends.
cp "$fixtures/input/swap_000.jp2" "$work/sw ap.jp2"
cp "$fixtures/input/swap_001.jp2" "$work/q\"x.jp2"
{
    printf ' \t -i %s,"%s"' "$(printf '%s' "$work/sw ap.jp2" | sed 's/ /\\ /g')" \
        "$(printf '%s' "$work/q\"x.jp2" | sed 's/"/\\"/')"
    for f in $rest; do printf '  %s' "$f"; done
    printf '\n\n  -o %s \t\n\n' "$work/quoted.jpx"
} >"$work/quoted-args"
[ "$(status "$exe" -s "$work/quoted-args")" = 0 ] ||
    fail "escapes in the argument file: $(cat "$work/stderr")"
cmp -s "$work/quoted.jpx" "$expected" || fail "escapes in the argument file: other bytes"
# An unterminated quote, either kind, or a final backslash: nothing read.
for bad in "-i 'a.jp2" '-i "a.jp2' '-i a.jp2\'; do
    printf '%s' "$bad" >"$work/bad-args"
    [ "$(status "$exe" -o "$work/bad.jpx" -s "$work/bad-args")" = 1 ] &&
        grep -q "cannot read the arguments in" "$work/stderr" ||
        fail "argument file $bad: expected 1"
done
[ ! -e "$work/bad.jpx" ] || fail "an unreadable argument file gave a merge"
rm -f "$work/sw ap.jp2" "$work/q\"x.jp2" "$work/quoted.jpx" "$work/quoted-args" "$work/bad-args"

# Inputs that are no regular file, or empty.
mkdir "$work/dir.jp2"
[ "$(status "$exe" -i "$work/dir.jp2" -o "$work/dir.jpx")" = 1 ] &&
    grep -q "not a regular file" "$work/stderr" || fail "a directory as input: expected 1"
rmdir "$work/dir.jp2"
: >"$work/empty.jp2"
[ "$(status "$exe" -i "$work/empty.jp2" -o "$work/empty.jpx")" = 1 ] ||
    fail "an empty input: expected 1"
[ ! -e "$work/empty.jpx" ] || fail "an empty input gave a merge"
rm -f "$work/empty.jp2"

[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -o "$work/links.jpx" -links)" = 0 ] ||
    fail "-links failed: $(cat "$work/stderr")"
[ -s "$work/links.jpx" ] || fail "-links wrote nothing"

# Arguments from standard input.
rm -f "$work/argfile.jpx"
[ "$(status "$exe" -s - <"$work/args")" = 0 ] || fail "-s - failed: $(cat "$work/stderr")"
cmp -s "$work/argfile.jpx" "$expected" || fail "-s - gives other bytes"

# An existing output keeps its mode; one that is a symbolic link keeps the
# link and gets its target replaced.
chmod 600 "$work/links.jpx"
ln -s links.jpx "$work/link.jpx"
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -o "$work/link.jpx" -links)" = 0 ] ||
    fail "-o through a link failed: $(cat "$work/stderr")"
[ -L "$work/link.jpx" ] || fail "the link was replaced by a file"
[ "$(ls -l "$work/links.jpx" | cut -c2-10)" = "rw-------" ] ||
    fail "the output's mode changed to $(ls -l "$work/links.jpx" | cut -c2-10)"
# A link to a file not there yet: the file is created, the link stays.
ln -s new.jpx "$work/new-link.jpx"
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -o "$work/new-link.jpx" -links)" = 0 ] ||
    fail "-o through a dangling link failed: $(cat "$work/stderr")"
[ -L "$work/new-link.jpx" ] && [ -f "$work/new.jpx" ] ||
    fail "a dangling link was not written through"
rm -f "$work/new-link.jpx" "$work/new.jpx"

# No merge may replace an input, through any name or link.
cp "$fixtures/input/swap_000.jp2" "$work/self.jp2"
ln -s self.jp2 "$work/self-link.jp2"
ln "$work/self.jp2" "$work/self-hard.jp2"
for output in "$work/self.jp2" "$work/self-link.jp2" "$work/self-hard.jp2"; do
    for mode in "" "-links" "--validate" "--transcode"; do
        for inputs in "$work/self.jp2" "$work/self-link.jp2" "$work/self-hard.jp2" \
            "$fixtures/input/swap_000.jp2,$work/self.jp2"; do
            # shellcheck disable=SC2086
            [ "$(status "$exe" -i "$inputs" -o "$output" $mode)" = 1 ] ||
                fail "merge over its input $output: expected 1"
            grep -q "is also the output" "$work/stderr" ||
                fail "merge over its input: $(cat "$work/stderr")"
            cmp -s "$work/self.jp2" "$fixtures/input/swap_000.jp2" &&
                cmp -s "$output" "$fixtures/input/swap_000.jp2" ||
                fail "merge over its input changed it"
            [ -z "$(find "$work" -name 'self*.jp2.*' -print)" ] ||
                fail "merge over its input left a temporary file"
        done
    done
done
rm -f "$work/self.jp2" "$work/self-link.jp2" "$work/self-hard.jp2"

# An output that cannot be created: the message names it, not the
# temporary file.
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -o "$work/no-dir/out.jpx")" = 1 ] ||
    fail "no directory: expected 1"
grep -q "cannot create $work/no-dir/out.jpx: " "$work/stderr" &&
    ! grep -q 'out\.jpx\.' "$work/stderr" ||
    fail "no directory: $(cat "$work/stderr")"

# Failures: exit status 1, a message naming the input, no output, no
# temporary file.
head -c 2000 "$fixtures/input/swap_000.jp2" >"$work/cut.jp2"
[ "$(status "$exe" -i "$work/cut.jp2" -o "$work/cut.jpx")" = 1 ] ||
    fail "truncated file: expected 1"
grep -q "cut.jp2" "$work/stderr" || fail "the message does not name the input"
[ ! -e "$work/cut.jpx" ] || fail "a failed merge wrote its output"
[ "$(status "$exe" -i "$work/missing.jp2" -o "$work/missing.jpx")" = 1 ] ||
    fail "missing file: expected 1"
[ "$(status "$exe" -i "$transcode_fixtures/input/synthetic_rgb_129x129_origin129_CPRL.jp2" \
    --validate -o "$work/origin.jpx")" = 1 ] ||
    fail "nonzero origins: expected 1"
grep -q "siz.zero-origin" "$work/stderr" ||
    fail "nonzero origins: the message does not name the rule"
[ "$(status "$exe" -i "$transcode_fixtures/input/synthetic_rgb_129x129_origin129_CPRL.jp2" \
    -o "$work/origin.jpx")" = 0 ] || fail "default merge rejected non-profile origins"
[ "$(status "$exe" --validate -i "$fixtures/input/swap_000.jp2" \
    "$fixtures/input/swap_001.jp2" $rest -o "$work/validated.jpx")" = 0 ] ||
    fail "validation merge failed"
cmp -s "$work/validated.jpx" "$expected" || fail "validation output differs"
# PLT validation is optional in both linked and embedded output. A validation
# failure must preserve an existing output and remove its temporary file.
vectors="$fixtures/../../../tests/vectors/j2k"
for name in jp2-rule-plt.sum-exceeds-data-23.jp2 jp2-rule-plt.packet-count-45.jp2 \
    jp2-rule-plt.value-overflow-46.jp2 jp2-rule-plt.zero-length-44.jp2 \
    jp2-precincts-rule-plt.padding-position-43.jp2; do
    for links in '' '-links'; do
        # shellcheck disable=SC2086
        [ "$(status "$exe" $links -i "$vectors/$name" -o "$work/opaque.jpx")" = 0 ] ||
            fail "default rejected opaque $name: $(cat "$work/stderr")"
        printf 'existing output' >"$work/rejected.jpx"
        cp "$work/rejected.jpx" "$work/sentinel"
        # shellcheck disable=SC2086
        [ "$(status "$exe" --validate $links -i "$vectors/$name" \
            -o "$work/rejected.jpx")" = 1 ] || fail "validation accepted $name"
        cmp -s "$work/rejected.jpx" "$work/sentinel" || fail "validation failure changed output"
    done
done
# Interactions and precinct values are usage errors before any output is written.
for options in '-links --transcode' '--transcode --validate' '-p 128,128' \
    '--transcode -p 3,128' '--transcode -p 1,128' \
    '--transcode -p 65536,128' '--transcode -p 128' '--transcode -p 128,128x' \
    '--transcode -p 128,3' '--transcode -p 128,1' '--transcode -p 128,65536' \
    '--transcode -p 0,128' '--transcode -p 128,128,128' '--transcode -p ,128' \
    '--transcode -p 128,' '--transcode -p 999999999999999999999999,128'; do
    # shellcheck disable=SC2086
    [ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -o "$work/usage.jpx" $options)" = 2 ] ||
        fail "usage $options: expected 2"
done
# Precinct values are decimal digits only: no space, no sign.
for value in ' 128,128' '+128,128' '128, 128' '128,+128'; do
    [ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -o "$work/usage.jpx" \
        --transcode -p "$value")" = 2 ] || fail "-p '$value': expected 2"
done
[ ! -e "$work/usage.jpx" ] || fail "usage error created output"
mkdir "$work/inputs"
input="$work/inputs/first.jp2"
second_input="$work/inputs/second.jp2"
cp "$transcode_fixtures/input/2015_12_21__00_10_34_34__SDO_AIA_AIA_171.jp2" "$input"
cp "$transcode_fixtures/input/solo_fsi174_510x514_LRCP.jp2" "$second_input"
cp "$input" "$work/first-before.jp2"
cp "$second_input" "$work/second-before.jp2"
find "$work/inputs" -print | sort >"$work/inputs-before"
for precincts in 128,128 256,128; do
    [ "$(status "$transcode" -p "$precincts" "$input" "$work/frame.jp2")" = 0 ] || fail "prepare transcode"
    [ "$(status "$transcode" -p "$precincts" "$second_input" "$work/second-frame.jp2")" = 0 ] || fail "prepare second transcode"
    [ "$(status "$exe" --validate -i "$work/frame.jp2" "$work/second-frame.jp2" -o "$work/separate.jpx")" = 0 ] || fail "prepare merge"
    [ "$(status "$exe" --transcode -p "$precincts" -i "$input" "$second_input" -o "$work/direct.jpx")" = 0 ] ||
        fail "transcode merge: $(cat "$work/stderr")"
    cmp -s "$work/direct.jpx" "$work/separate.jpx" || fail "transcode pipeline bytes: $precincts"
    cmp -s "$input" "$work/first-before.jp2" || fail "transcode changed the first input"
    cmp -s "$second_input" "$work/second-before.jp2" || fail "transcode changed the second input"
    find "$work/inputs" -print | sort >"$work/inputs-after"
    cmp -s "$work/inputs-before" "$work/inputs-after" || fail "transcode changed the input directory"
done
printf -- '--transcode -p 256,128\n' >"$work/precinct-args"
[ "$(status "$exe" -i "$input" "$second_input" -s "$work/precinct-args" -o "$work/arg-precinct.jpx")" = 0 ] ||
    fail "argfile enables transcode and precincts"
cmp -s "$work/arg-precinct.jpx" "$work/direct.jpx" || fail "argfile precinct bytes"
# A missing input proves the boundary values pass CLI validation without
# depending on whether an image's code-block partition permits them.
for precincts in 2,2 32768,32768; do
    [ "$(status "$transcode" -p "$precincts" "$work/missing.jp2" "$work/boundary.jp2")" = 1 ] &&
        grep -q 'cannot open' "$work/stderr" || fail "transcode boundary precincts: $precincts"
    [ "$(status "$exe" --transcode -p "$precincts" -i "$work/missing.jp2" -o "$work/boundary.jpx")" = 1 ] &&
        grep -q 'cannot open' "$work/stderr" || fail "merge boundary precincts: $precincts"
done
printf -- '--transcode\n' >"$work/precinct-args"
[ "$(status "$exe" -i "$input" -o "$work/default-arg.jpx" -s "$work/precinct-args")" = 0 ] || fail "argfile defaults"
[ "$(status "$exe" --transcode -i "$input" -o "$work/default.jpx")" = 0 ] || fail "transcode defaults"
cmp -s "$work/default-arg.jpx" "$work/default.jpx" || fail "argfile precinct defaults differ"
printf -- '--validate\n' >"$work/precinct-args"
[ "$(status "$exe" --transcode -i "$input" -o "$work/usage.jpx" -s "$work/precinct-args")" = 2 ] || fail "argfile conflict"
[ ! -e "$work/usage.jpx" ] || fail "argfile conflict created output"
# Later input and transcode failures preserve an existing destination.
for mode in '' '--validate' '-links' '--transcode'; do
    printf 'existing output' >"$work/late.jpx"
    cp "$work/late.jpx" "$work/sentinel"
    # shellcheck disable=SC2086
    [ "$(status "$exe" $mode -i "$kakadu/2015_12_21__00_10_34_34__SDO_AIA_AIA_171.jp2" "$work/cut.jp2" -o "$work/late.jpx")" = 1 ] || fail "late bad input: $mode"
    grep -q 'cut.jp2' "$work/stderr" || fail "late error does not name input"
    cmp -s "$work/late.jpx" "$work/sentinel" || fail "late failure changed destination"
done
# A nonzero-origin source fails the served container check (siz.zero-origin).
bad="$transcode_fixtures/input/synthetic_rgb_129x129_origin129_CPRL.jp2"
[ "$(status "$exe" --transcode -i "$input" "$bad" -o "$work/late.jpx")" = 1 ] || fail "late transcode accepted"
grep -q 'origin129' "$work/stderr" || fail "transcode error does not name input"
cmp -s "$work/late.jpx" "$work/sentinel" || fail "late transcode changed destination"

leftover=$(ls "$work" | grep -c '\.jpx\.' || true)
[ "$leftover" = 0 ] || fail "temporary files left behind: $(ls "$work")"

rm -rf "$work"
[ "$failures" = 0 ] || exit 1
echo "hv_merge command: all checks passed"
