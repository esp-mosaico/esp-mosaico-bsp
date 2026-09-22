#!/usr/bin/env python3
from pathlib import Path
import struct
import sys
import zlib

def chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))

target = Path(sys.argv[1])
row = b"\x00" + bytes((92, 190, 236)) * 480
target.parent.mkdir(parents=True, exist_ok=True)
target.write_bytes(b"".join((b"\x89PNG\r\n\x1a\n",
    chunk(b"IHDR", struct.pack(">IIBBBBB", 480, 480, 8, 2, 0, 0, 0)),
    chunk(b"IDAT", zlib.compress(row * 480, 9)), chunk(b"IEND", b""))))
