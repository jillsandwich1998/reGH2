"""Writes a plain 256x256 PNG icon (red disc on black) without any imaging library."""
import struct
import sys
import zlib

N = 256
rows = []
for y in range(N):
    row = bytearray([0])
    for x in range(N):
        d = ((x - N / 2) ** 2 + (y - N / 2) ** 2) ** 0.5
        if d < 100:
            c = (204, 28, 26) if d > 70 else (255, 212, 64) if d < 28 else (10, 6, 6)
        else:
            c = (10, 6, 6)
        row += bytes(c + (255,))
    rows.append(bytes(row))
raw = b"".join(rows)


def chunk(t, d):
    c = struct.pack(">I", len(d)) + t + d
    return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
