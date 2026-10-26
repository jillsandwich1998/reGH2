#!/usr/bin/env python3
"""Reference for gh2_vfs_selftest's ARK manifest: one line per entry, path, offset, size, crc32 of the entry's bytes, from ark_extract.py's reader.

  ark_manifest.py GAME_DIR_OR_HDR OUT.tsv      then:  diff OUT.tsv <manifest written by gh2_vfs_selftest GAME_DIR MANIFEST>
"""
import pathlib
import sys
import zlib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import ark_extract as A  # noqa: E402


def main():
    hdr = A.locate(sys.argv[1])
    version, encrypted, sizes, entries = A.parse_header(hdr.read_bytes())
    with open(sys.argv[2], "w") as out:
        for e in entries:
            data = A.read_entry(hdr, e)
            out.write("%s\t%d\t%d\t%08x\n" % (e.path, e.offset, len(data), zlib.crc32(data) & 0xFFFFFFFF))
    print("%d entries, version %d, encrypted %s" % (len(entries), version, encrypted))


if __name__ == "__main__":
    main()
