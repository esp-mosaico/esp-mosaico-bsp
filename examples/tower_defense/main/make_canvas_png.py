#!/usr/bin/env python3
"""Create the exact opaque 480x480 placeholder required by GSP Canvas."""
from pathlib import Path
import struct
import sys
import zlib

def chunk(kind: bytes, payload: bytes) -> bytes:
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff))

def main() -> int:
    target = Path(sys.argv[1])
    width = height = 480
    row = b"\x00" + bytes((8, 16, 27)) * width
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(b"".join((
        b"\x89PNG\r\n\x1a\n",
        chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)),
        chunk(b"IDAT", zlib.compress(row * height, 9)),
        chunk(b"IEND", b""))))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
