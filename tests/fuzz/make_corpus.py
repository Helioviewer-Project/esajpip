#!/usr/bin/env python3
"""Build per-target fuzz seed corpora from the checked-in test vectors."""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
from pathlib import Path


JP2C = b"jp2c"
# Boxes whose payload is boxes, as far as the seeds need to look into them.
SUPERBOXES = {b"jp2h", b"res ", b"jpch", b"jplh", b"ftbl", b"asoc", b"uinf", b"cgrp", b"comp", b"drep"}
# The asn1 seeds: the encodings start where a box, a box's payload, a marker,
# a segment's length or a segment's body starts in these files, cut to
# WINDOW bytes (more than any type's T_REQUIRED_BYTES_FOR_ACN_ENCODING).
ASN1_SOURCES = ("jp2.jp2", "jp2-precincts.jp2", "jpx-embedded.jpx", "jpx-linked.jpx")
WINDOW = 256
ACN_DECODE = re.compile(rb"^flag [A-Za-z0-9_]+_ACN_Decode\(", re.M)


def write_unique(directory: Path, prefix: str, data: bytes) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256(data).hexdigest()[:16]
    (directory / f"{prefix}-{digest}").write_bytes(data)


def copy_seed(path: Path, directory: Path, prefix: str) -> None:
    data = path.read_bytes()
    if data:
        write_unique(directory, prefix, data)


def jp2c_payloads(data: bytes) -> list[bytes]:
    payloads: list[bytes] = []
    pos = 0
    end = len(data)
    while pos + 8 <= end:
        box_start = pos
        length = int.from_bytes(data[pos : pos + 4], "big")
        box_type = data[pos + 4 : pos + 8]
        pos += 8

        if length == 1:
            if pos + 8 > end:
                break
            length = int.from_bytes(data[pos : pos + 8], "big")
            pos += 8
            payload_start = pos
        elif length == 0:
            payload_start = pos
            pos = end
            if box_type == JP2C and payload_start <= end:
                payloads.append(data[payload_start:end])
            break
        else:
            payload_start = pos

        if length < payload_start - box_start:
            break
        box_end = box_start + length
        if box_end > end:
            break
        if box_type == JP2C:
            payloads.append(data[payload_start:box_end])
        pos = box_end
    return [payload for payload in payloads if payload]


def asn1_types(repo: Path) -> int:
    """The number of PDU types, counted as tests/fuzz/CMakeLists.txt lists
    them in asn1_pdus.h."""
    return sum(len(ACN_DECODE.findall(header.read_bytes()))
               for header in sorted((repo / "jpeg2000" / "generated").glob("*.h")))


def codestream_windows(data: bytes, start: int, end: int, out: set[bytes]) -> None:
    """The main header's and the first tile-part header's structures."""
    pos = start
    if data[pos : pos + 2] != b"\xff\x4f":
        return
    out.add(data[pos : pos + WINDOW])
    pos += 2
    while pos + 4 <= end:
        code = int.from_bytes(data[pos : pos + 2], "big")
        out.add(data[pos : pos + WINDOW])
        if code in (0xFF93, 0xFFD9):  # SOD, EOC
            return
        length = int.from_bytes(data[pos + 2 : pos + 4], "big")
        out.add(data[pos + 2 : pos + 2 + WINDOW])
        out.add(data[pos + 4 : pos + 4 + WINDOW])
        if code == 0xFF58:  # PLT: its entries, after Zplt
            out.add(data[pos + 5 : pos + 5 + WINDOW])
        if length < 2:
            return
        pos += 2 + length


def box_windows(data: bytes, start: int, end: int, out: set[bytes], depth: int = 0) -> None:
    pos = start
    while pos + 8 <= end:
        box_start = pos
        length = int.from_bytes(data[pos : pos + 4], "big")
        box_type = data[pos + 4 : pos + 8]
        pos += 8
        if length == 1:
            if pos + 8 > end:
                break
            length = int.from_bytes(data[pos : pos + 8], "big")
            pos += 8
        elif length == 0:
            length = end - box_start
        out.add(data[box_start : box_start + WINDOW])
        if length < pos - box_start or box_start + length > end:
            break
        out.add(data[pos : pos + WINDOW])
        if box_type == JP2C:
            codestream_windows(data, pos, box_start + length, out)
        elif box_type == b"dtbl":  # NDR, then url boxes
            out.add(data[pos + 2 : pos + 2 + WINDOW])
            box_windows(data, pos + 2, box_start + length, out, depth + 1)
        elif box_type in SUPERBOXES and depth < 4:
            box_windows(data, pos, box_start + length, out, depth + 1)
        pos = box_start + length


def asn1_windows(data: bytes) -> set[bytes]:
    out: set[bytes] = set()
    if data.startswith(b"\xff\x4f"):
        codestream_windows(data, 0, len(data), out)
    else:
        box_windows(data, 0, len(data), out)
    return {window for window in out if window}


def corpus_paths(repo: Path) -> dict[str, list[Path]]:
    return {
        "vectors": sorted((repo / "tests" / "vectors" / "j2k").glob("*.jp*")),
        "transcode": sorted((repo / "tests" / "transcode" / "fixtures" / "input").glob("*.jp2")),
        "merge": sorted((repo / "tests" / "merge" / "fixtures" / "input").glob("*.jp2")),
    }


def jpip_corpora(out: Path) -> None:
    requests = (
        b"", b"/jpip?cid=7", b"/movie.jpx?cnew=http&type=jpp-stream&len=512",
        b"/jpip?fsiz=1024,1024,closest&stream=0-&layers=1&cid=7",
        b"/jpip?fsiz=4096,4096,round-up&roff=-2,1&rsiz=9,3&cid=7",
        b"/jpip?fsiz=12,34,sideways&cid=8", b"/jpip?context=jpxl%3C0-%3E&cid=7",
        b"/jpip?stream=1-9:2,0-3&context=jpxl<0->&model=P0&cid=7",
        b"/jpip?model=%5B1-2%5DHm:3,H0:5,P6:7,M0:9&tid=0&cid=7",
        b"/jpip?model=[-5-9]P0&cid=7", b"/jpip?model=[9-3]P0&cid=7",
        b"/jpip?model=[0-]P0&cid=7", b"/jpip?model=M0%00M1&cid=7",
        b"/jpip?model=-M0&cid=7", b"/jpip?model=P0:L1&cid=7",
        b"/jpip?model=X0&cid=7", b"/jpip?model=M0%&cid=7",
        b"/jpip?len=2147483648&layers=18446744073709551616&cid=7",
        b"/jpip?cid=7&cid=8&model=M0&tid=1", b"/jpip?cnew=http&cclose=7",
    )
    for request in requests:
        write_unique(out / "jpip-request", "query", request)
    # Writer input: capacity selector, then flags/class/stream/bin/offset/length/source-offset.
    # High capacity selectors carry a two-byte capacity. Boundary selectors below
    # 128 select the table in jpip_support.h; high ones carry a uint64.
    for capacity in (134, 135, 16410, 32784):
        for length in (4, 8):  # 126 and 16382 bytes, then two adjacent bytes.
            body = bytes([1, 2, 0, 0, 0, length, 0]) + bytes([17, 2, 0, 0, 0, 1, 0]) * 2
            write_unique(out / "jpip-writer", "coalesce", b"\x80" + struct.pack(">H", capacity) + body)
    for capacity in range(17):
        for boundary in range(15):
            body = bytes([1, 0, 255, boundary, boundary, boundary % 11, 0,
                          97, 3, 0, 0, 0, 7, 0, 7, 1, 2, 0])
            write_unique(out / "jpip-writer", "boundaries", bytes([capacity]) + body)
    # Cache operations: opcode/class/stream/id/amount/complete.
    for stream in range(4):
        body = b"".join(bytes([4, 3, stream, i, 1, 1]) for i in reversed(range(32)))
        body += bytes([0, 3, stream, 0, 0, 0, 4, 3, stream, 32, 1, 0])
        write_unique(out / "jpip-cache", "complete-prefix", body)
    for cls in range(7):
        for amount in range(7):
            body = bytes([4, cls, 1, 255, amount, 0, 3, cls, 1, 255, 6, 0,
                          0, cls, 1, 255, 0, 0, 1, 0, 0, 0, 0, 0])
            write_unique(out / "jpip-cache", "update", body)
    # Session header: progression/JPX/padding. Each request then has eleven
    # controls: mode/selector/fsiz-x/fsiz-y/roff-x/roff-y/rsiz-x/rsiz-y/layers/len/capacity.
    viewport = bytes([1, 0, 2, 1, 2, 2, 0, 0, 1, 9, 0])
    zoom = bytes([65, 9, 4, 2, 2, 2, 5, 3, 3, 11, 1])
    crop = bytes([9, 0, 4, 2, 6, 2, 1, 3, 2, 8, 2])
    reconnect = bytes([33, 9, 4, 2, 2, 2, 5, 3, 3, 12, 3])
    for progression in range(5):
        for jpx in range(2):
            for padding in (0, 7):
                header = bytes([progression, jpx, padding])
                write_unique(out / "jpip-session", "full", header)
                write_unique(out / "jpip-session", "history", header + viewport + crop + zoom + reconnect)
                for budget in range(13):
                    write_unique(out / "jpip-session", "budget", header +
                                 bytes([1, 0, 4, 2, 2, 2, 5, 3, 3, budget, 0]))
    for mode in range(8):
        record = bytes([17 + mode * 2, 0, 4, 2, 2, 2, 5, 3, 3, 12, 0])
        write_unique(out / "jpip-session", "invalid-model", b"\x00\x01\x00" + record)
    write_unique(out / "jpip-session", "invalid-window", b"\x00\x01\x00" +
                 bytes([129, 0, 4, 2, 2, 2, 5, 3, 3, 12, 0]))
    for mode in ("jpip-request", "jpip-writer", "jpip-cache", "jpip-session"):
        write_unique(out / mode, "empty", b"")


def build_corpora(repo: Path, out: Path) -> None:
    jpip_corpora(out)
    paths = corpus_paths(repo)
    # Campaigns add inputs here too. Refresh seeds without deleting discoveries.
    out.mkdir(parents=True, exist_ok=True)

    reader = out / "reader-rewrite"
    deferred = out / "deferred-plt"
    asn1 = out / "asn1"
    transcode = out / "transcode-fuzz"
    merge = out / "merge-fuzz"

    for path in paths["vectors"]:
        prefix = path.stem.replace(".", "-")
        copy_seed(path, reader, prefix)
        data = path.read_bytes()
        if data.startswith(b"\xff\x4f"):
            write_unique(deferred, prefix, data)
            write_unique(transcode, prefix, data)
        for index, payload in enumerate(jp2c_payloads(data)):
            write_unique(deferred, f"{prefix}-jp2c-{index}", payload)
            write_unique(transcode, f"{prefix}-jp2c-{index}", payload)

    for path in paths["transcode"]:
        prefix = path.stem.replace(".", "-")
        copy_seed(path, reader, prefix)
        for index, payload in enumerate(jp2c_payloads(path.read_bytes())):
            write_unique(deferred, f"{prefix}-jp2c-{index}", payload)
            write_unique(transcode, f"{prefix}-jp2c-{index}", payload)

    # fuzz_asn1's first byte selects the type: every window with every type.
    types = asn1_types(repo)
    windows: set[bytes] = set()
    for name in ASN1_SOURCES:
        windows |= asn1_windows((repo / "tests" / "vectors" / "j2k" / name).read_bytes())
    for selector in range(min(types, 256)):
        for window in windows:
            write_unique(asn1, f"type-{selector}", bytes([selector]) + window)

    merge_inputs = [path.read_bytes() for path in paths["merge"] if path.stat().st_size > 0]
    for left_index, left in enumerate(merge_inputs):
        for right_index, right in enumerate(merge_inputs):
            payload = struct.pack(">I", len(left)) + left + right
            write_unique(merge, f"merge-{left_index}-{right_index}", payload)
            for exponent in (8, 9):
                # fuzz_merge.c transcodes at exponent data[0] % 15 + 1, so a top
                # byte of exponent - 1 selects it; keep the split modulo the
                # pair's size while changing that byte.
                encoded = (exponent - 1) << 24
                encoded += (len(left) - encoded) % (len(left) + len(right) + 1)
                assert encoded >> 24 == exponent - 1
                assert encoded % (len(left) + len(right) + 1) == len(left)
                payload = struct.pack(">I", encoded) + left + right
                write_unique(merge, f"merge-{left_index}-{right_index}-precinct-{exponent}", payload)
    # One valid input and one of the JP2 vectors, each of which breaks one
    # rule of that same file, in both orders.
    valid = (repo / "tests" / "vectors" / "j2k" / "jp2.jp2").read_bytes()
    for path in paths["vectors"]:
        if path.suffix != ".jp2" or path.name == "jp2.jp2":
            continue
        other = path.read_bytes()
        prefix = path.stem.replace(".", "-")
        write_unique(merge, f"valid-{prefix}", struct.pack(">I", len(valid)) + valid + other)
        write_unique(merge, f"{prefix}-valid", struct.pack(">I", len(other)) + other + valid)

    for seed in (b"", b"\x00", b"\xff", b"\xff\x4f", b"\x00\x00\x00\x0cjP  "):
        write_unique(reader, "tiny", seed)
        write_unique(deferred, "tiny", seed)
        write_unique(asn1, "tiny", seed)
        write_unique(transcode, "tiny", seed)
        write_unique(merge, "tiny", seed)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("repo", type=Path)
    parser.add_argument("out", type=Path)
    args = parser.parse_args()
    build_corpora(args.repo.resolve(), args.out.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
