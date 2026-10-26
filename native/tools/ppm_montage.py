#!/usr/bin/env python3
"""ppm_montage.py OUT.png COLUMNS SCALE IN.ppm [IN.ppm ...]    sheet of the PPM frames GH2_RENDER_DUMP writes (P6), each shrunk by SCALE (integer), as a PNG.
Standard library only. Also: ppm_montage.py OUT.png 1 1 IN.ppm   converts one picture."""
import struct
import sys
import zlib


def read(path):
    raw = open(path, "rb").read()
    fields, pos = [], 0
    while len(fields) < 4:                          # P6, width, height, maxval: separated by any whitespace, one more after maxval
        while raw[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while end < len(raw) and not raw[end:end + 1].isspace():
            end += 1
        if end == pos:
            raise SystemExit(path + ": truncated PPM header (a dump cut off by the end of the run?)")
        fields.append(raw[pos:end])
        pos = end
    return int(fields[1]), int(fields[2]), raw[pos + 1:]


def main():
    out, cols, scale = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    tiles = []
    for path in sys.argv[4:]:
        w, h, pix = read(path)
        tw, th = w // scale, h // scale
        rows = [b"".join(pix[((y * scale) * w + x * scale) * 3:((y * scale) * w + x * scale) * 3 + 3] for x in range(tw)) for y in range(th)]
        tiles.append((tw, th, rows))
    tw = max(t[0] for t in tiles)
    th = max(t[1] for t in tiles)
    nrows = (len(tiles) + cols - 1) // cols
    width, height = cols * tw + (cols - 1) * 4, nrows * th + (nrows - 1) * 4
    sheet = [bytearray(b"\x40\x40\x40" * width) for _ in range(height)]
    for i, (w, h, rows) in enumerate(tiles):
        ox, oy = (i % cols) * (tw + 4), (i // cols) * (th + 4)
        for y, row in enumerate(rows):
            sheet[oy + y][ox * 3:ox * 3 + len(row)] = row
    raw = b"".join(b"\x00" + bytes(r) for r in sheet)

    def chunk(t, c):
        x = struct.pack(">I", len(c)) + t + c
        return x + struct.pack(">I", zlib.crc32(t + c) & 0xFFFFFFFF)

    open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 3)) + chunk(b"IEND", b""))
    print(len(tiles), width, height)


main()
