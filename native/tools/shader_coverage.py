#!/usr/bin/env python3
"""Do the shaders the game really uses at run time exist in the extracted set? (the "unknown hash" question, answered with real data)

  shader_coverage.py FX_XBOX_DIR DUMP_DIR [--vertex-diff]

FX_XBOX_DIR  the extracted .fx_xbox files (shader_pipeline.py stage S.0)
DUMP_DIR     a directory written by the PoC run with `--dump_shaders=DIR`: shader_<hash>.ucode.bin.(vert|frag) = the microcode the
             game submitted to the GPU (the SDK's own hash, not ours)
Every container's microcode (container + virtualSize + shader.physicalOffset, shader.size bytes) is compared with every dump, trailing
zero words ignored, in guest byte order and with every 32-bit word swapped. Exit status 1 when a dumped shader has no container.
--vertex-diff: for dumped vertex shaders without an identical container, finds the container of the same size that differs in the fewest
32-bit words and reports how many (the Xbox 360 D3D library patches the vertex fetch instructions of a vertex shader to the vertex
declaration when it is bound, so the microcode that reaches the GPU is not the one in the file; pixel shaders are not patched).
"""
import collections
import hashlib
import pathlib
import struct
import sys


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def container_ucode(data):
    """Microcode of a shader container, or None if it is not one (same checks as XenosRecomp / ShaderCache::ParseContainerHeader)."""
    if len(data) < 36 or (be32(data, 0) & 0xFFFFFF00) != 0x102A1100:
        return None
    virtual_size, physical_size, shader_offset = be32(data, 4), be32(data, 8), be32(data, 24)
    if be32(data, 28) != 0 or be32(data, 32) != 0 or virtual_size + physical_size > len(data):
        return None
    physical_offset, size = be32(data, shader_offset), be32(data, shader_offset + 4)
    start = virtual_size + physical_offset
    if start + size > len(data):
        return None
    stage = "ps" if (be32(data, 0) & 1) == 0 else "vs"
    return stage, data[start:start + size]


def strip_zero_words(b):
    n = len(b) // 4 * 4
    while n >= 4 and b[n - 4:n] == b"\0\0\0\0":
        n -= 4
    return b[:n]


def swap_words(b):
    n = len(b) // 4 * 4
    return b"".join(b[i:i + 4][::-1] for i in range(0, n, 4))


def fingerprint(b):
    return hashlib.sha1(strip_zero_words(b)).hexdigest()


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 2:
        sys.exit(__doc__)
    fx, dump = pathlib.Path(args[0]), pathlib.Path(args[1])
    containers = {}
    for f in sorted(fx.glob("*.fx_xbox")):
        r = container_ucode(f.read_bytes())
        if r:
            containers.setdefault(fingerprint(r[1]), (r[0], f.name))
    print("containers: %d distinct microcodes from %d files" % (len(containers), len(list(fx.glob("*.fx_xbox")))))
    res = {"vs": [0, 0, []], "ps": [0, 0, []]}
    order = {"as-is": 0, "word-swapped": 0}
    for f in sorted(dump.glob("shader_*.ucode.bin.*")):
        stage = "vs" if f.name.endswith(".vert") else "ps"
        raw = f.read_bytes()
        hit = None
        for label, blob in (("as-is", raw), ("word-swapped", swap_words(raw))):
            fp = fingerprint(blob)
            if fp in containers:
                hit = (label, containers[fp])
                break
        res[stage][1] += 1
        if hit:
            res[stage][0] += 1
            order[hit[0]] += 1
        else:
            res[stage][2].append((f.name.split(".")[0].replace("shader_", ""), len(strip_zero_words(raw))))
    tot_hit = sum(v[0] for v in res.values())
    tot = sum(v[1] for v in res.values())
    for st, name in (("vs", "vertex"), ("ps", "pixel")):
        print("%-6s dumped %3d, found in the extracted set %3d (%s)" % (name, res[st][1], res[st][0],
                                                                         "%.1f%%" % (100.0 * res[st][0] / res[st][1]) if res[st][1] else "n/a"))
    print("total  dumped %3d, found %3d (%.1f%%); byte order that matched: %s" % (tot, tot_hit, 100.0 * tot_hit / tot if tot else 0, order))
    unmatched = [(st, h, n) for st in res for (h, n) in res[st][2]]
    if unmatched:
        print("NOT in the extracted set (%d): %s" % (len(unmatched), ", ".join("%s %s (%d B)" % u for u in unmatched[:6]) +
                                                    (" ..." if len(unmatched) > 6 else "")))
    ps_missing = [u for u in unmatched if u[0] == "ps"]
    if ps_missing:
        print("pixel shaders NOT in the extracted set: " + ", ".join("%s (%d B)" % (h, n) for _, h, n in ps_missing))
    if "--vertex-diff" in sys.argv:
        vs_by_size = collections.defaultdict(set)
        for f in sorted(fx.glob("*.fx_xbox")):
            r = container_ucode(f.read_bytes())
            if r and r[0] == "vs":
                u = strip_zero_words(r[1])
                vs_by_size[len(u)].add(u)
        words = lambda b: struct.unpack(">%dI" % (len(b) // 4), b)
        counts, positions, no_same_size, identical = collections.Counter(), collections.Counter(), 0, 0
        for st, h, n in unmatched:
            if st != "vs":
                continue
            raw = strip_zero_words(swap_words((dump / ("shader_%s.ucode.bin.vert" % h)).read_bytes()))
            best = None
            for c in vs_by_size.get(len(raw), ()):
                a, b = words(raw), words(c)
                diff = [i for i in range(len(a)) if a[i] != b[i]]
                if best is None or len(diff) < len(best):
                    best = diff
            if best is None:
                no_same_size += 1
                continue
            identical += not best
            counts[len(best)] += 1
            for i in best:
                positions[i % 3] += 1
        total = sum(counts.values())
        print("vertex shaders without an identical container: %d have a same-size container (%d have none)" % (total, no_same_size))
        print("  32-bit words that differ from the closest container: %s" % dict(sorted(counts.items())))
        print("  position of a differing word inside the 3-word instruction (0,1,2): %s" % dict(sorted(positions.items())))
    sys.exit(1 if unmatched else 0)


if __name__ == "__main__":
    main()
