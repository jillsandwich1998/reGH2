#!/usr/bin/env python3
"""Packs the SPIR-V modules XenosRecomp emitted into the shader cache of native/gfx/shader_cache (shaders.pak + manifest.json + index.tsv).

  shader_cache_build.py EMIT_DIR CACHE_DIR [--provenance JSON]

EMIT_DIR is the directory written by `XenosRecomp ... --emit EMIT_DIR` (<hash>.spv and index.tsv). Shaders whose status is not "ok" are
left out (the index keeps them, with the reason). The format is documented in native/gfx/shader_cache/shader_cache.h and is read back by
gh2_shader_cache_tool (verify / selftest), which is the cross-check that this writer and the C++ reader agree.
"""
import csv
import hashlib
import json
import pathlib
import struct
import sys
import time
import zlib

MAGIC = b"GH2SHDR1"
VERSION = 2
HEADER = struct.Struct("<8sIIIIII")        # magic, version, count, entries_offset, blob_offset, blob_size, blob_crc32
ENTRY = struct.Struct("<QIIIBBH")          # hash, offset, size, spec_mask, stage, flags, dep_mask (vertex shaders: elements read by dependent fetches)


def build(emit_dir, cache_dir, provenance=None, excluded=()):
    emit, out = pathlib.Path(emit_dir), pathlib.Path(cache_dir)
    out.mkdir(parents=True, exist_ok=True)
    rows = list(csv.DictReader(open(emit / "index.tsv"), delimiter="\t"))
    entries, blob = [], bytearray()
    skipped = {}
    for r in sorted(rows, key=lambda r: int(r["hash"], 16)):
        if r["status"] != "ok" or r["hash"] in excluded:
            skipped[r["hash"]] = r["status"] if r["status"] != "ok" else "excluded"
            continue
        spv = (emit / (r["hash"] + ".spv")).read_bytes()
        if len(spv) % 4 or spv[:4] != b"\x03\x02\x23\x07":
            skipped[r["hash"]] = "not a SPIR-V module"
            continue
        while len(blob) % 4:
            blob.append(0)
        entries.append((int(r["hash"], 16), len(blob), len(spv), int(r["spec_mask"]), 0 if r["stage"] == "vs" else 1, 0, int(r.get("dep_mask") or 0) & 0xFFFF))
        blob += spv
    entries_offset = HEADER.size
    blob_offset = entries_offset + ENTRY.size * len(entries)
    header = HEADER.pack(MAGIC, VERSION, len(entries), entries_offset, blob_offset, len(blob), zlib.crc32(bytes(blob)) & 0xFFFFFFFF)
    pak = bytearray(header)
    for e in entries:
        pak += ENTRY.pack(*e)
    pak += blob
    (out / "shaders.pak").write_bytes(pak)
    (out / "index.tsv").write_bytes((emit / "index.tsv").read_bytes())
    manifest = {
        "format": "gh2 shader cache",
        "format_version": VERSION,
        "hash": "XXH3_64bits over the shader container (virtualSize + physicalSize bytes)",
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "provenance": provenance or {},
        "stats": {
            "containers_in_index": len(rows),
            "shaders_in_pak": len(entries),
            "vertex": sum(1 for e in entries if e[4] == 0),
            "pixel": sum(1 for e in entries if e[4] == 1),
            "spirv_bytes": len(blob),
            "pak_bytes": len(pak),
            "pak_sha1": hashlib.sha1(pak).hexdigest(),
            "left_out": skipped,
        },
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    return manifest


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        sys.exit(__doc__)
    prov = None
    if "--provenance" in sys.argv:
        prov = json.loads(pathlib.Path(sys.argv[sys.argv.index("--provenance") + 1]).read_text())
    m = build(args[0], args[1], prov)
    s = m["stats"]
    print("cache: %d shaders (%d vertex, %d pixel), %.2f MB of SPIR-V, pak %.2f MB, %d left out" %
          (s["shaders_in_pak"], s["vertex"], s["pixel"], s["spirv_bytes"] / 1e6, s["pak_bytes"] / 1e6, len(s["left_out"])))


if __name__ == "__main__":
    main()
