#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Write vcjpeg.datatype's descriptor (SYS:Storage/DataTypes/VCJPEG).

An IFF FORM DTYP: NAME, FVER, DTHD and DTCD.  DTHD is struct DataTypeHeader
(datatypes/datatypes.h) with its string pointers as offsets from the start of
the chunk's data, the strings after the 32-byte header.  DTCD is the
recognition function, a hunk file (vcjpeg_recog.c) that AddDataTypes loads
with the descriptor.

No mask, priority 0: datatypes.library 47 tries descriptors with a function
and no mask before the others; among them the lowest priority first (0 is
the lowest: priority cannot put VCJPEG ahead of AmiKit's JFIF, which has 0
too) and, at equal priority, the one added last first.  So VCJPEG goes ahead
of JFIF and OS 3.2's JPEG when it is added after them (from S:User-Startup;
docs/architecture.md section 8).

The file is parsed back before it is kept: a malformed one fails the build.

Usage: descriptor.py <output file> <recognition function> <version> <date>
"""
import struct
import sys

NAME = "VCJPEG"
BASENAME = "vcjpeg"           # the class: datatypes/vcjpeg.datatype
GROUP = b"pict"               # GID_PICTURE
IDENT = b"jpeg"               # what OS 3.2's JPEG descriptor says: programs that ask learn it is a JPEG
PATTERN = "#?"
DTF_BINARY = 0x0000
PRIORITY = 0
HUNK_HEADER = 0x000003F3
HUNK_CODE = 0x000003E9
DO_NOT_EXECUTE = 0x70FF4E75   # a module's first word (moveq #-1,d0; rts): the function must not start so


def chunk(cid, data):
    """An IFF chunk: id, size, data, padded to an even length."""
    pad = b"\0" if len(data) & 1 else b""
    return cid + struct.pack(">I", len(data)) + data + pad


def dthd():
    """struct DataTypeHeader with its strings: name, base name, pattern, no mask."""
    o_name = 32
    o_base = o_name + len(NAME) + 1
    o_pattern = o_base + len(BASENAME) + 1
    head = struct.pack(">IIIIIIhhHH", o_name, o_base, o_pattern, 0, int.from_bytes(GROUP, "big"), int.from_bytes(IDENT, "big"),
                       0, 0, DTF_BINARY, PRIORITY)
    return head + b"".join(s.encode("ascii") + b"\0" for s in (NAME, BASENAME, PATTERN))


def check_function(code):
    """The function's hunk file: a header, one code hunk, the function at its start."""
    if len(code) < 32 or struct.unpack(">I", code[:4])[0] != HUNK_HEADER:
        sys.exit("descriptor.py: the recognition function is no hunk file")
    # the header: its magic, the resident library names (none: one 0 long-word), the hunk count, first and last
    # hunk, one size per hunk
    if struct.unpack(">I", code[4:8])[0] != 0:
        sys.exit("descriptor.py: the recognition function's hunk header names resident libraries")
    hunks = struct.unpack(">I", code[8:12])[0]
    first = 20 + 4 * hunks  # behind the header's sizes: the first hunk's type, its length, its code
    if struct.unpack(">I", code[first:first + 4])[0] != HUNK_CODE:
        sys.exit("descriptor.py: the recognition function's first hunk is no code")
    if struct.unpack(">I", code[first + 8:first + 12])[0] == DO_NOT_EXECUTE:
        sys.exit("descriptor.py: the recognition function starts with a module's stub, not with itself")


def build(code, version, date):
    """The whole file."""
    body = b"DTYP" + chunk(b"NAME", NAME.encode("ascii") + b"\0")
    body += chunk(b"FVER", f"$VER: {NAME} {version} {date}".encode("ascii") + b"\0")
    body += chunk(b"DTHD", dthd())
    body += chunk(b"DTCD", code)
    return b"FORM" + struct.pack(">I", len(body)) + body


def parse(data):
    """The chunks of a FORM DTYP, and its header's fields: a malformed file raises."""
    assert data[:4] == b"FORM" and data[8:12] == b"DTYP", "no FORM DTYP"
    assert struct.unpack(">I", data[4:8])[0] == len(data) - 8, "FORM size"
    chunks, pos = {}, 12
    while pos < len(data):
        cid, size = data[pos:pos + 4], struct.unpack(">I", data[pos + 4:pos + 8])[0]
        chunks[cid] = data[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    assert pos == len(data), "chunk sizes"
    d = chunks[b"DTHD"]
    o_name, o_base, o_pattern, o_mask, group, ident, mask_len, pad, flags, pri = struct.unpack(">IIIIIIhhHH", d[:32])

    def string(offset):
        return d[offset:d.index(b"\0", offset)].decode("ascii")
    return chunks, dict(name=string(o_name), basename=string(o_base), pattern=string(o_pattern), mask=o_mask, mask_len=mask_len,
                        pad=pad, group=group.to_bytes(4, "big"), ident=ident.to_bytes(4, "big"), flags=flags, priority=pri)


def main():
    """Build the file, parse it back, write it."""
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    code = open(sys.argv[2], "rb").read()
    check_function(code)
    data = build(code, sys.argv[3], sys.argv[4])
    chunks, h = parse(data)
    assert chunks[b"NAME"] == NAME.encode("ascii") + b"\0" and chunks[b"DTCD"] == code
    assert (h["name"], h["basename"], h["pattern"]) == (NAME, BASENAME, PATTERN)
    assert (h["mask"], h["mask_len"], h["pad"], h["flags"], h["priority"]) == (0, 0, 0, DTF_BINARY, PRIORITY)
    assert (h["group"], h["ident"]) == (GROUP, IDENT)
    with open(sys.argv[1], "wb") as f:
        f.write(data)


if __name__ == "__main__":
    main()
