#!/bin/sh
# The hv_walk command: options, exit status and the line it prints for
# each outcome, on corpus vectors and on files made here.
#   walk_test.sh HV_WALK VECTORS WORK
set -eu
exe=$1
vectors=$2
work=$3
rm -rf "$work"
mkdir -p "$work"
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

# expect STATUS TEXT COMMAND...: the exit status, and TEXT in the output.
expect() {
    want=$1
    text=$2
    shift 2
    got=$(status "$@")
    [ "$got" = "$want" ] || fail "$*: exit status $got, expected $want"
    grep -F -q -- "$text" "$work/stdout" ||
        fail "$*: no \"$text\" in: $(cat "$work/stdout")"
}

# The big-endian 32-bit value $1, as bytes.
be32() {
    printf "\\$(printf %03o $((($1 >> 24) & 255)))\\$(printf %03o $((($1 >> 16) & 255)))"
    printf "\\$(printf %03o $((($1 >> 8) & 255)))\\$(printf %03o $(($1 & 255)))"
}

# Usage errors.
[ "$(status "$exe")" = 2 ] || fail "no arguments: expected exit status 2"
[ "$(status "$exe" -x "$vectors/jp2.jp2")" = 2 ] || fail "unknown option: expected 2"
[ "$(status "$exe" -p -P "$vectors/jp2.jp2")" = 2 ] || fail "-p with -P: expected 2"
[ "$(status "$exe" -R "$work" "$vectors/jp2.jp2")" = 2 ] || fail "-R without -P: expected 2"
[ "$(status "$exe" -P -R)" = 2 ] || fail "-R without a directory: expected 2"
grep -q '^usage: hv_walk' "$work/stderr" || fail "a usage error printed no usage"

# One line per file; the status is 1 when any file fails.
expect 0 "jp2.jp2: 6 boxes, 1 codestreams, 1 tile-parts" "$exe" "$vectors/jp2.jp2"
expect 1 "ERROR   $work/missing.jp2: cannot read" \
    "$exe" "$vectors/jp2.jp2" "$work/missing.jp2"
grep -q '^valid   .*jp2.jp2' "$work/stdout" || fail "a later failure hid an earlier file"

# -v: the boxes, and the codestream items with their marker codes.
expect 0 "  jp2c [77, 173)" "$exe" -v "$vectors/jp2.jp2"
grep -q '^    tile-segment FF58 \[162, 168)$' "$work/stdout" || fail "-v listed no PLT"

# A raw codestream: read as such, and failed by -P and -H, which check files.
dd if="$vectors/jp2.jp2" of="$work/raw.j2k" bs=1 skip=85 2>/dev/null
expect 0 "raw.j2k: 0 boxes, 1 codestreams, 1 tile-parts" "$exe" "$work/raw.j2k"
expect 1 "raw.j2k: file.signature at 0" "$exe" -H "$work/raw.j2k"
expect 1 "raw.j2k: file.signature at 0" "$exe" -P "$work/raw.j2k"

# The layers: jp2-sig-bad.jp2 only fails the file rules.
expect 0 "jp2-sig-bad.jp2: 6 boxes" "$exe" "$vectors/jp2-sig-bad.jp2"
expect 1 "jp2-sig-bad.jp2: file.signature at 0" "$exe" -P "$vectors/jp2-sig-bad.jp2"
expect 0 "jpx-embedded.jpx: 12 boxes, 2 codestreams, 0 tile-parts" \
    "$exe" -H "$vectors/jpx-embedded.jpx"

# Header-only validation leaves unrelated quantization and tile data opaque.
expect 0 "; headers only" "$exe" -H "$vectors/jp2-rule-qcd.style-72.jp2"
expect 1 "qcd.style" "$exe" "$vectors/jp2-rule-qcd.style-72.jp2"
expect 1 "qcd.style" "$exe" -H -w "$vectors/jp2-rule-qcd.style-72.jp2"

# Trailing zero PLT entries: refused at the standard layer, accepted with -p
# and counted.
plt=$vectors/jp2-rule-plt.trailing-zero-16.jp2
expect 1 "plt.zero-length at 150" "$exe" "$plt"
expect 0 "1 tile-parts, 1 zero PLT entries" "$exe" -p "$plt"

# -P on a linked JPX file: its linked codestreams counted; outside -R, or
# failing in the linked file, named with that file.
expect 0 "15 boxes, 0 codestreams, 0 tile-parts, 2 linked codestreams" \
    "$exe" -P "$vectors/jpx-linked.jpx"
expect 1 "linked file outside the root directory at 250 (linked file" \
    "$exe" -P -R "$work" "$vectors/jpx-linked.jpx"
expect 1 "flst.source-extent at 85 of" \
    "$exe" -P "$vectors/jpx-linked-rule-flst.source-extent-12.jpx"

# -w: rewritten identically, or not compared, with the reason.
expect 0 "1 tile-parts; rewritten identically" "$exe" -w "$vectors/jp2.jp2"
expect 0 "; rewrite not compared: LBox = 0 is written as a length" \
    "$exe" -w "$vectors/jpx-embedded-len-20-23.jpx"
expect 0 "; rewrite not compared: zero PLT entries are dropped" \
    "$exe" -w "$vectors/jp2-rule-tile.packed-headers-91.jp2"

# Superboxes nested one level deeper than hv_walk follows (33 asoc boxes,
# each the only content of the one before).
: >"$work/deep.jp2"
level=33
while [ "$level" -gt 0 ]; do
    { be32 $((8 * level)); printf asoc; } >>"$work/deep.jp2"
    level=$((level - 1))
done
expect 1 "superboxes nested deeper than 32 at 256" "$exe" "$work/deep.jp2"

if [ "$failures" -ne 0 ]; then
    echo "$failures failure(s); files in $work" >&2
    exit 1
fi
rm -rf "$work"
