#!/bin/sh
# The hv_merge command: options as hvJP2K's, exit status, and nothing
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
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -o "$work/out.jpx" -links=yes)" = 2 ] ||
    fail "-links=yes: expected 2"
[ ! -e "$work/out.jpx" ] || fail "a usage error wrote the output"
[ "$(status "$exe" -q -h)" = 0 ] && grep -q "^usage: hv_merge" "$work/stdout" ||
    fail "-h: expected the usage on standard output and exit status 0"
# No names left once the commas are dropped: hvJP2K's error.
[ "$(status "$exe" -i , -o "$work/out.jpx")" = 1 ] && grep -q "no JP2 input files" "$work/stderr" ||
    fail "only commas: expected 1 and \"no JP2 input files\""
[ ! -e "$work/out.jpx" ] || fail "a failed merge wrote the output"
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -o "")" = 1 ] || fail "-o \"\": expected 1"

# Comma and space separated names, stray commas included.
# shellcheck disable=SC2086
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2,,$fixtures/input/swap_001.jp2," $rest \
    -o "$work/out.jpx")" = 0 ] || fail "merge failed: $(cat "$work/stderr")"
cmp -s "$work/out.jpx" "$expected" || fail "the output differs from hvJP2K's"

# Arguments from a file, quoted, after those of the command line.
{
    printf -- "-i '%s' \"%s\"" "$fixtures/input/swap_000.jp2" "$fixtures/input/swap_001.jp2"
    for f in $rest; do printf ' %s' "$f"; done
    printf '\n-o %s\n' "$work/argfile.jpx"
} >"$work/args"
[ "$(status "$exe" -s "$work/args")" = 0 ] || fail "-s failed: $(cat "$work/stderr")"
cmp -s "$work/argfile.jpx" "$expected" || fail "-s gives other bytes"

# As argparse reparses: a later -i replaces an earlier one, the command
# line's included; a -s in the file is parsed but not read.
{
    printf -- "-i %s" "$fixtures/input/swap_000.jp2,$fixtures/input/swap_001.jp2"
    for f in $rest; do printf ' %s' "$f"; done
    printf ' -s %s\n' "$work/missing-args"
} >"$work/later-args"
[ "$(status "$exe" -i "$fixtures/input/swap_001.jp2" -o "$work/later.jpx" \
    -s "$work/later-args")" = 0 ] ||
    fail "-i and -s in the argument file: $(cat "$work/stderr")"
cmp -s "$work/later.jpx" "$expected" || fail "-i in the argument file: other bytes"
# Only space, tab, line feed and carriage return separate words, as in
# shlex: "-links" and a form feed make an unknown option.
printf -- '-i %s -o %s -links\f\n' "$fixtures/input/swap_000.jp2" "$work/ff.jpx" >"$work/ff-args"
[ "$(status "$exe" -s "$work/ff-args")" = 2 ] || fail "a form feed in the argument file: expected 2"
[ ! -e "$work/ff.jpx" ] || fail "a form feed in the argument file: the output was written"
# Bare names in the file continue a final -i of the command line.
for f in $rest; do printf '%s\n' "$f"; done >"$work/names"
[ "$(status "$exe" -o "$work/bare.jpx" -s "$work/names" -i "$fixtures/input/swap_000.jp2" \
    "$fixtures/input/swap_001.jp2")" = 0 ] || fail "bare names in the file: $(cat "$work/stderr")"
cmp -s "$work/bare.jpx" "$expected" || fail "bare names in the file: other bytes"
# Values that start with -: "-" alone and negative numbers; attached values
# and abbreviations.
ln -s "$fixtures/input/swap_000.jp2" "$work/-1"
(cd "$work" && "$exe" -i -1 -o neg.jpx 2>/dev/null) || fail "-i -1: a file named -1 not merged"
[ "$(status "$exe" "-i$work/-1" "-o$work/attached.jpx" -lin)" = 0 ] ||
    fail "-iNAME -oNAME -lin: $(cat "$work/stderr")"
[ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -o "$work/full.jpx" -links)" = 0 ] &&
    cmp -s "$work/attached.jpx" "$work/full.jpx" || fail "-iNAME -oNAME -lin: other bytes"
rm -f "$work/-1" "$work/neg.jpx" "$work/attached.jpx" "$work/full.jpx" "$work/bare.jpx" \
    "$work/later.jpx"

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

# shlex's quoting in the argument file: a backslash outside quotes, a
# quote escaped inside double quotes, runs of blanks and blanks at the ends.
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

# -h with letters attached: help once the option it carries is taken;
# with "=" or a "-", argparse's ignored explicit argument.
[ "$(status "$exe" -hi "$fixtures/input/swap_000.jp2")" = 0 ] &&
    grep -q "^usage: hv_merge" "$work/stdout" || fail "-hi NAME: expected help"
[ "$(status "$exe" -hx)" = 0 ] && grep -q "^usage: hv_merge" "$work/stdout" ||
    fail "-hx: expected help"
[ "$(status "$exe" -h=x)" = 2 ] || fail "-h=x: expected 2"
[ "$(status "$exe" -h-x)" = 2 ] || fail "-h-x: expected 2"
[ "$(status "$exe" -hi)" = 2 ] || fail "-hi without a name: expected 2"
# A negative number with a fraction is a value, as -1 is.
ln -s "$fixtures/input/swap_000.jp2" "$work/-1.5"
(cd "$work" && "$exe" -i -1.5 -o frac.jpx 2>/dev/null) || fail "-i -1.5: a file named -1.5 not merged"
rm -f "$work/-1.5" "$work/frac.jpx"

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
[ "$(status "$exe" --validate=yes -i "$fixtures/input/swap_000.jp2" \
    -o "$work/bad-option.jpx")" = 2 ] || fail "--validate takes no value"
[ "$(status "$exe" --h -i "$fixtures/input/swap_000.jp2" \
    -o "$work/bad-option.jpx")" = 0 ] || fail "--h means help"
[ "$(status "$exe" --he)" = 0 ] || fail "--he still means help"
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
# Interactions are checked after the argument file is reparsed.
for options in '-links --transcode' '--transcode --validate' '-p 128,128' \
    '--transcode=yes' '--transcode -p 3,128' '--transcode -p 1,128' \
    '--transcode -p 65536,128' '--transcode -p 128' '--transcode -p 128,128x'; do
    # shellcheck disable=SC2086
    [ "$(status "$exe" -i "$fixtures/input/swap_000.jp2" -o "$work/usage.jpx" $options)" = 2 ] ||
        fail "usage $options: expected 2"
done
[ ! -e "$work/usage.jpx" ] || fail "usage error created output"
[ "$(status "$exe" -hp 128,128)" = 0 ] || fail "-hp: expected help"
[ "$(status "$exe" -hp)" = 2 ] || fail "-hp without value: expected 2"
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
printf -- '--transcode -p=256,128\n' >"$work/precinct-args"
[ "$(status "$exe" -p128,128 -i "$input" "$second_input" -o "$work/arg-precinct.jpx" -s "$work/precinct-args")" = 0 ] ||
    fail "argfile enables transcode and overrides precincts"
cmp -s "$work/arg-precinct.jpx" "$work/direct.jpx" || fail "argfile precinct override bytes"
printf -- '--transcode\n' >"$work/precinct-args"
[ "$(status "$exe" -i "$input" -o "$work/default-arg.jpx" -s "$work/precinct-args")" = 0 ] || fail "argfile defaults"
[ "$(status "$exe" --transcode -i "$input" -o "$work/default.jpx")" = 0 ] || fail "transcode defaults"
cmp -s "$work/default-arg.jpx" "$work/default.jpx" || fail "argfile precinct defaults not restored"
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
