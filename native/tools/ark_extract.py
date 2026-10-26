#!/usr/bin/env python3
"""Reads the player's own Harmonix ARK archive (gen/main.hdr + main_N.ark) and extracts files from it. Nothing from the game is
stored in the repository: this tool runs on the player's machine (installer step S.0) and writes into a directory the caller picks.

  ark_extract.py HDR_OR_GEN_DIR --summary
  ark_extract.py HDR_OR_GEN_DIR --list [--match REGEX]
  ark_extract.py HDR_OR_GEN_DIR --extract OUT_DIR --match REGEX      (keeps the archive's directory layout)

Format (written from the public description of the format, e.g. the MIT-licensed Mackiloha / ArkHelper project, then reimplemented):
  main.hdr = int32 version, or, when that value is not a known version, a 32-bit KEY followed by a stream encrypted with a
  Park-Miller generator (key' = (key mod 127773) * 16807 - (key div 127773) * 2836, +2^31-1 when <= 0, one keystream byte per
  data byte); the decrypted stream starts with the real version. Versions 3-7 then hold: part sizes, a string blob + string index
  table, and one entry per file (offset, name index, directory index, size, inflated size). Entry offsets are global across the
  numbered parts main_0.ark, main_1.ark ... The paths inside start with "../.." (the engine maps ".." to "(..)"); extraction does the same
  and never writes outside OUT_DIR. Exit status 2 = unsupported or corrupt header.
"""
import argparse
import os
import pathlib
import re
import struct
import sys
import zlib

KNOWN_VERSIONS = {2, 3, 4, 5, 6, 7, 9, 10}


class ArkError(Exception):
    pass


def wrap32(x):
    return ((x + 0x80000000) & 0xFFFFFFFF) - 0x80000000


def next_key(data):
    """One step of the header keystream (signed 32-bit arithmetic, C-style truncating division)."""
    q = abs(data) // 0x1F31D
    q = q if data >= 0 else -q
    value = wrap32((data - q * 0x1F31D) * 0x41A7 - q * 0xB14)
    return wrap32(value + 0x7FFFFFFF) if value <= 0 else value


def decrypt(buf, key):
    out = bytearray(len(buf))
    for i, b in enumerate(buf):
        key = next_key(key)
        out[i] = b ^ (key & 0xFF)
    return bytes(out)


class Reader:
    def __init__(self, data, pos=0):
        self.d, self.p = data, pos

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.p)[0]
        self.p += 4
        return v

    def i32(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]
        self.p += 4
        return v

    def i64(self):
        v = struct.unpack_from("<q", self.d, self.p)[0]
        self.p += 8
        return v

    def cstr(self):
        end = self.d.index(b"\0", self.p)
        s = self.d[self.p:end].decode("latin-1")
        self.p = end + 1
        return s

    def lstr(self):
        n = self.u32()
        s = self.d[self.p:self.p + n].decode("latin-1")
        self.p += n
        return s


class Entry:
    __slots__ = ("path", "offset", "size", "inflated", "part", "part_offset")


def parse_header(raw):
    """Returns (version, encrypted, part_sizes, entries)."""
    version = struct.unpack_from("<i", raw, 0)[0]
    encrypted = False
    data = raw
    if version not in KNOWN_VERSIONS:
        encrypted = True
        key = version
        data = raw[:4] + decrypt(raw[4:], key)
        version = struct.unpack_from("<i", data, 4)[0]
        if version not in KNOWN_VERSIONS:
            version = (version ^ 0xFFFFFFFF) & 0xFFFFFFFF
            if version not in KNOWN_VERSIONS:
                raise ArkError("unsupported ARK version after decryption (%d)" % version)
            data = data[:8] + bytes(b ^ 0xFF for b in data[8:])
        r = Reader(data, 8)
    else:
        r = Reader(data, 4)
    if version > 7:
        raise ArkError("ARK version %d is not supported by this reader (3-7 are)" % version)
    if version >= 6:
        r.p += r.u32() << 4
    part_count = r.u32()
    size_count = r.u32()
    start = r.p
    broken_v4 = False
    if version != 4:
        sizes = [r.u32() for _ in range(size_count)]
    else:
        sizes = [r.i64() for _ in range(size_count)]
        if sizes and sizes[-1] > 0xFFFFFFFF:        # a v3/v5 hybrid some builds ship
            broken_v4 = True
            r.p = start
            sizes = [r.u32() for _ in range(size_count)]
    if version >= 5 or broken_v4:
        for _ in range(r.u32()):
            r.lstr()
    if version >= 6:
        r.p += r.u32() << 2
    if version >= 7:
        for _ in range(r.u32()):
            for _ in range(r.u32()):
                r.lstr()
    blob_size = r.u32()
    blob_start = r.p
    strings = {}
    while r.p - blob_start < blob_size:
        off = r.p - blob_start
        strings[off] = r.cstr()
    index = [r.i32() for _ in range(r.u32())]
    count = r.u32()
    wide = version >= 4 and not broken_v4
    entries = []
    for _ in range(count):
        e = Entry()
        e.offset = r.i64() if wide else r.u32()
        name = strings[index[r.i32()]]
        directory = strings[index[r.i32()]]
        e.size, e.inflated = r.u32(), r.u32()
        e.path = (directory + "/" + name) if directory else name
        # which part holds it
        base, e.part = 0, 0
        while e.part < len(sizes) - 1 and e.offset >= base + sizes[e.part]:
            base += sizes[e.part]
            e.part += 1
        e.part_offset = e.offset - base
        entries.append(e)
    return version, encrypted, sizes, entries


def locate(arg):
    p = pathlib.Path(arg)
    if p.is_dir():
        cands = sorted(p.glob("*.hdr")) or sorted((p / "gen").glob("*.hdr"))
        if not cands:
            raise ArkError("no .hdr file in %s" % p)
        p = cands[0]
    return p


def safe_relpath(path):
    """The archive's paths start with ../.. (the engine maps ".." to "(..)" on the host); keep every component inside the output
    directory the same way and refuse empty / absolute leftovers."""
    parts = [("(..)" if c == ".." else c) for c in path.replace("\\", "/").split("/") if c not in ("", ".")]
    if not parts:
        raise ArkError("empty entry path")
    return pathlib.PurePosixPath(*parts)


def part_path(hdr, index):
    return hdr.with_name("%s_%d.ark" % (hdr.stem, index))


def read_entry(hdr, e):
    with open(part_path(hdr, e.part), "rb") as f:
        f.seek(e.part_offset)
        data = f.read(e.size)
    if len(data) != e.size:
        raise ArkError("short read for %s" % e.path)
    if e.inflated and e.inflated != e.size:
        data = zlib.decompress(data, -15) if not data[:2] in (b"\x78\x9c", b"\x78\x01", b"\x78\xda") else zlib.decompress(data)
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("source", help="main.hdr or the gen/ directory that holds it")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--extract", metavar="OUT_DIR")
    ap.add_argument("--match", default="", help="regular expression on the entry path")
    a = ap.parse_args()
    hdr = locate(a.source)
    try:
        version, encrypted, sizes, entries = parse_header(hdr.read_bytes())
    except (ArkError, struct.error, IndexError, KeyError, ValueError) as e:
        print("ark_extract: %s" % e, file=sys.stderr)
        return 2
    rx = re.compile(a.match) if a.match else None
    chosen = [e for e in entries if not rx or rx.search(e.path)]
    if a.summary or not (a.list or a.extract):
        ext = {}
        for e in entries:
            k = os.path.splitext(e.path)[1].lower() or "(none)"
            ext[k] = ext.get(k, 0) + 1
        compressed = sum(1 for e in entries if e.inflated and e.inflated != e.size)
        print("ARK %s: version %d, header %s, %d part(s) %s, %d entries (%d compressed)" %
              (hdr.name, version, "encrypted" if encrypted else "plain", len(sizes),
               "/".join(str(s) for s in sizes), len(entries), compressed))
        print("extensions: " + ", ".join("%s=%d" % kv for kv in sorted(ext.items(), key=lambda kv: -kv[1])[:14]))
        missing = [i for i in range(len(sizes)) if not part_path(hdr, i).exists()]
        print("parts present on disk: %s" % ("all" if not missing else "MISSING %s" % missing))
    if a.list:
        for e in chosen:
            print("%10d %s" % (e.size, e.path))
    if a.extract:
        out = pathlib.Path(a.extract)
        total = 0
        root = out.resolve()
        for e in chosen:
            dest = out / safe_relpath(e.path)
            if not dest.resolve().is_relative_to(root):          # defense in depth: never write outside OUT_DIR
                raise ArkError("refusing to write outside %s: %s" % (out, e.path))
            dest.parent.mkdir(parents=True, exist_ok=True)
            data = read_entry(hdr, e)
            dest.write_bytes(data)
            total += len(data)
        print("extracted %d files (%.1f MB) to %s" % (len(chosen), total / 1e6, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
