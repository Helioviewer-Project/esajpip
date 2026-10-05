#!/usr/bin/env python3
"""Independent byte-level cases for the standards review's confirmed defects."""

import struct
import subprocess
import sys
from pathlib import Path

walk, merge, transcode = sys.argv[1:4]
repo, work = map(Path, sys.argv[4:6])
work.mkdir(parents=True, exist_ok=True)
failures = []
checks = 0


def box(kind, payload):
    return struct.pack('>I4s', len(payload) + 8, kind) + payload


def boxes(data):
    pos = 0
    while pos < len(data):
        size, kind = struct.unpack_from('>I4s', data, pos)
        assert size >= 8 and pos + size <= len(data)
        yield kind, data[pos + 8:pos + size]
        pos += size


def change_box(data, kind, payload):
    return b''.join(box(t, payload if t == kind else p) for t, p in boxes(data))


def payload(data, kind):
    return next(p for t, p in boxes(data) if t == kind)


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=30)


def check(ok, message):
    global checks
    checks += 1
    if not ok:
        failures.append(message)


def read(name, data, reason=None, flags=()):
    path = work / name
    path.write_bytes(data)
    result = run([walk, *flags, str(path)])
    check(result.returncode == (1 if reason else 0) and
          (reason is None or reason in result.stdout + result.stderr),
          f'{name}: expected {reason or "accepted"}, got {result.stdout}{result.stderr}')
    return path


base = (repo / 'tests/vectors/j2k/jp2.jp2').read_bytes()
# Header inspection treats JPX-only bodies as opaque in a JP2 container.
read('opaque-header-extension.jp2', base + box(b'asoc', b'x'), flags=('-H',))

cs = payload(base, b'jp2c')
sot = cs.index(b'\xff\x90')
part = cs[sot:-2]

# B.11 and Table A.5: all tiles must be represented and addressable.
for name, xsiz, ysiz, xtsiz, ytsiz, reason in (
        ('missing-horizontal-tile.jp2', 8, 4, 4, 4, 'codestream.tile-count'),
        ('missing-vertical-tile.jp2', 4, 8, 4, 4, 'codestream.tile-count'),
        ('65536-tiles.jp2', 256, 256, 1, 1, 'siz.tile-count'),
        ('huge-grid.jp2', 0xffffffff, 0xffffffff, 1, 1, 'siz.tile-count')):
    changed = bytearray(cs)
    struct.pack_into('>II', changed, 8, xsiz, ysiz)
    struct.pack_into('>II', changed, 24, xtsiz, ytsiz)
    ihdr = bytearray(payload(payload(base, b'jp2h'), b'ihdr'))
    struct.pack_into('>II', ihdr, 0, ysiz, xsiz)
    header = change_box(payload(base, b'jp2h'), b'ihdr', ihdr)
    candidate = change_box(change_box(base, b'jp2h', header), b'jp2c', changed)
    read(name, candidate, reason)
    if name == 'missing-horizontal-tile.jp2':
        second = bytearray(part)
        struct.pack_into('>H', second, 4, 1)
        complete = bytes(changed[:sot]) + part + second + b'\xff\xd9'
        read('complete-two-tiles.jp2', change_box(candidate, b'jp2c', complete))
        repeated = bytes(changed[:sot]) + part + part + b'\xff\xd9'
        read('repeated-tile-is-not-coverage.jp2', change_box(candidate, b'jp2c', repeated),
             'sot.tpsot-sequence')

# A.7.1--A.7.5: gaps are allowed, duplicates are not. The bytes after Z
# remain unchanged; the payload lengths and packed-header coverage are exact.
psot = len(part)
for z in (1, 7, 255):
    tlm = b'\xff\x55' + struct.pack('>HBBH', 6, z, 0, psot)
    plm = b'\xff\x57' + struct.pack('>HBB', 5, z, 1) + b'\x01'
    for marker, segment, reason in (('tlm', tlm, 'tlm.index'),
                                     ('plm', plm, 'plm.index')):
        read(f'{marker}-gap-{z}.j2k', cs[:sot] + segment + cs[sot:])
        read(f'{marker}-duplicate-{z}.j2k', cs[:sot] + segment * 2 + cs[sot:], reason)
    changed = bytearray(cs)
    changed[cs.index(b'\xff\x58') + 4] = z
    read(f'plt-gap-{z}.j2k', changed)
    read(f'plt-profile-{z}.jp2', change_box(base, b'jp2c', changed),
         'plt.zplt-sequence', ('-P',))

# With a packed empty header, the tile data are empty and its PLT length zero.
packed_part = bytearray(part[:-1])
plt = packed_part.index(b'\xff\x58')
packed_part[plt + 5] = 0
struct.pack_into('>I', packed_part, 6, len(packed_part))
ppm = b'\xff\x60\x00\x08\xff\x00\x00\x00\x01\x00'
read('ppm-gap-255.j2k', cs[:sot] + ppm + packed_part + b'\xff\xd9')
read('ppm-duplicate.j2k', cs[:sot] + ppm * 2 + packed_part + b'\xff\xd9', 'ppm.index')
ppt = b'\xff\x61\x00\x04\xff\x00'
ppt_part = packed_part[:-2] + ppt + b'\xff\x93'
ppt_part = bytearray(ppt_part)
struct.pack_into('>I', ppt_part, 6, len(ppt_part))
read('ppt-gap-255.j2k', cs[:sot] + ppt_part + b'\xff\xd9')
duplicate = bytearray(ppt_part[:-2] + ppt + b'\xff\x93')
struct.pack_into('>I', duplicate, 6, len(duplicate))
read('ppt-duplicate.j2k', cs[:sot] + duplicate + b'\xff\xd9', 'ppt.index')

# Main-header series are traversed by index, despite physical permutation
# and a large gap. Explicit TLM tile IDs distinguish the two entries.
two_main = bytearray(cs[:sot])
struct.pack_into('>I', two_main, 8, 8)
second_part = bytearray(part)
struct.pack_into('>H', second_part, 4, 1)
tlm_low = b'\xff\x55' + struct.pack('>HBBBH', 7, 1, 0x10, 0, len(part))
tlm_high = b'\xff\x55' + struct.pack('>HBBBH', 7, 255, 0x10, 1, len(part))
read('tlm-permuted-gap.j2k', two_main + tlm_high + tlm_low + part + second_part + b'\xff\xd9')
ppm_low = b'\xff\x60\x00\x08\x01\x00\x00\x00\x01\x00'
packed_second = bytearray(packed_part)
struct.pack_into('>H', packed_second, 4, 1)
read('ppm-permuted-gap.j2k', two_main + ppm + ppm_low + packed_part + packed_second + b'\xff\xd9')
plm_low = b'\xff\x57\x00\x04\x01\x01'
plm_high = b'\xff\x57\x00\x04\xff\x01'
read('plm-permuted-gap.j2k', cs[:sot] + plm_high + plm_low + cs[sot:])

# I.5.3.6: complete channel and color coverage, not merely N entries.
rgb = bytearray((repo / 'tests/transcode/fixtures/input/synthetic_rgb_129x129_CPRL_SOP_EPH.jp2').read_bytes())
rgb[rgb.index(b'\xff\x52') + 8] = 0  # no MCT requiring RGB channel order
header = payload(rgb, b'jp2h')
for name, definitions, reason in (
        ('cdef-missing-channels.jp2', [(0, 0, 3)], 'cdef.complete'),
        ('cdef-missing-color.jp2', [(0, 0, 3), (1, 65535, 65535), (2, 0, 1)], 'cdef.complete'),
        ('cdef-duplicate-count.jp2', [(0, 0, 3)] * 3, 'cdef.complete'),
        ('cdef-complete.jp2', [(0, 0, 3), (1, 0, 1), (2, 0, 2)], None),
        ('cdef-complete-with-duplicate.jp2', [(0, 0, 3), (1, 0, 1), (2, 0, 2), (0, 0, 3)], None)):
    definitions = struct.pack('>H', len(definitions)) + b''.join(struct.pack('>HHH', *e) for e in definitions)
    read(name, change_box(rgb, b'jp2h', header + box(b'cdef', definitions)), reason, ('-H',))

# With CMAP, completeness is measured in mapped channels, not NC.
palette = box(b'pclr', b'\x00\x01\x04' + b'\x07' * 4 + b'\x00' * 4)
channel_map = box(b'cmap', b''.join(struct.pack('>HBB', 0, 1, c) for c in range(4)))
for count in (3, 4):
    entries = [(0, 0, 1)] + [(c, 65535, 65535) for c in range(1, count)]
    cdef = box(b'cdef', struct.pack('>H', count) + b''.join(struct.pack('>HHH', *e) for e in entries))
    mapped = change_box(base, b'jp2h', payload(base, b'jp2h') + palette + channel_map + cdef)
    read(f'cdef-palette-{count}.jp2', mapped, 'cdef.complete' if count == 3 else None, ('-H',))

# Tools ignore input MinV/PREC/APPROX, then emit conforming values. Their
# output is identical to the same transformation of the canonical input.
canonical = work / 'canonical.jp2'
canonical.write_bytes(base)
modified = bytearray(base)
ftyp = modified.index(b'ftyp') + 4
struct.pack_into('>I', modified, ftyp + 4, 0xffffffff)
colr = modified.index(b'colr') + 4
modified[colr + 1:colr + 3] = b'\xff\xff'
ignored = read('ignored-input-fields.jp2', modified, 'file.ftyp-minor', ('-H',))
for name, exe, suffix in (('merge', merge, '.jpx'), ('transcode', transcode, '.jp2')):
    outputs = []
    for source in (canonical, ignored):
        out = work / (source.stem + '-' + name + suffix)
        args = [exe, '-i', str(source), '-o', str(out)] if name == 'merge' else [exe, str(source), str(out)]
        result = run(args)
        check(result.returncode == 0, f'{name} {source.name}: {result.stdout}{result.stderr}')
        if result.returncode == 0:
            read(out.name, out.read_bytes(), flags=('-H',))
            outputs.append(out.read_bytes())
    check(len(outputs) == 2 and outputs[0] == outputs[1], f'{name}: ignored fields changed output')
check(ignored.read_bytes() == modified, 'tool mutated input')

for failure in failures:
    print('FAIL:', failure)
print(f'{checks} checks, {len(failures)} failures')
sys.exit(bool(failures))
