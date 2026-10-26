#!/usr/bin/env python3
"""Reads a window recording of the D3D discovery probe (GH2_D3D_PROBE=<prefix> GH2_D3D_PROBE_WINDOW=<first frame>:<frames>, see gh2gfx/d3d_probe.h).

  d3d_window_report.py PREFIX functions              every function called in the window: calls per frame, threads, PM4 words it wrote and which packets
  d3d_window_report.py PREFIX sequence [FRAME] [N] [ADDR [K]]  the first N calls (default 80) of one frame in call order, nested by depth, with arguments and PM4 packets;
                                                      with ADDR, from the K-th (default 1st) call of that function on
  d3d_window_report.py PREFIX toplevel [FRAME] [THREAD] the calls the game itself makes into the block (depth 0) in one frame, in order, with the PM4 words each wrote
  d3d_window_report.py PREFIX function ADDR [N] [all]  N calls (default 6) of one function in the window frames (all: any recorded frame) with its arguments,
                                                      the memory behind each pointer and the PM4 it wrote
  d3d_window_report.py PREFIX draws                  every DRAW packet of the window, grouped by the innermost function that wrote it: kind, primitive, index source, counts
  d3d_window_report.py PREFIX swaps                  the frame marks (VdSwap) with the front buffer fetch constant decoded
  d3d_window_report.py PREFIX counts                 the exact number of calls of each function over the whole run
PM4 and texture/vertex fetch constants are decoded with the Xenos layouts (facts about the hardware, as documented by the Xenia and AMD sources).
"""
import collections
import os
import pathlib
import struct
import sys

EVENT = struct.Struct("<IHBB6II6II")
DUMP = struct.Struct("<IBBH64s")
PM4_HEADER = struct.Struct("<III")
FRAME_MARK = 0xFFFF

FULL = bool(os.environ.get("PM4_FULL"))        # PM4_FULL=1: print every word of a packet instead of the first few
PM4_OPS = {
    0x10: "NOP", 0x21: "REG_RMW", 0x22: "DRAW_INDX", 0x23: "VIZ_QUERY", 0x25: "SET_STATE?", 0x26: "WAIT_FOR_IDLE", 0x27: "IM_LOAD", 0x2B: "IM_LOAD_IMMEDIATE",
    0x2D: "SET_CONSTANT", 0x2F: "LOAD_ALU_CONSTANT", 0x36: "DRAW_INDX_2", 0x37: "INDIRECT_BUFFER_PFD", 0x3B: "INVALIDATE_STATE", 0x3C: "WAIT_REG_MEM",
    0x3D: "MEM_WRITE", 0x3E: "REG_TO_MEM", 0x3F: "INDIRECT_BUFFER", 0x40: "CP_INTERRUPT", 0x45: "COND_WRITE", 0x46: "EVENT_WRITE", 0x48: "ME_INIT",
    0x52: "WAIT_REG_EQ", 0x53: "WAIT_REG_GTE", 0x55: "SET_CONSTANT2", 0x56: "SET_SHADER_CONSTANTS", 0x58: "EVENT_WRITE_SHD", 0x5A: "EVENT_WRITE_EXT",
    0x5B: "EVENT_WRITE_ZPD", 0x60: "SET_BIN_MASK_LO", 0x64: "XE_SWAP",
}
CONST_TYPES = {0: "ALU", 1: "FETCH", 2: "BOOL", 3: "LOOP", 4: "REGISTER"}


def h(v):
    return "%08X" % v


def load(prefix):
    funcs = {int(a): int(b, 16) for a, b in (line.split() for line in open(prefix + ".functions"))}
    data = pathlib.Path(prefix + ".events").read_bytes()
    events = []
    for o in range(0, len(data) - EVENT.size + 1, EVENT.size):
        e = EVENT.unpack_from(data, o)
        events.append({"seq": e[0], "fn": e[1], "phase": e[2], "thread": e[3], "args": e[4:10], "ret": e[10], "deref": e[11:17], "frame": e[17]})
    dumps = collections.defaultdict(list)
    p = pathlib.Path(prefix + ".dumps")
    if p.exists():
        d = p.read_bytes()
        for o in range(0, len(d) - DUMP.size + 1, DUMP.size):
            seq, phase, arg, _, b = DUMP.unpack_from(d, o)
            dumps[seq].append((phase, arg, b))
    pm4 = {}
    p = pathlib.Path(prefix + ".pm4")
    if p.exists():
        d = p.read_bytes()
        o = 0
        while o + PM4_HEADER.size <= len(d):
            seq, first, n = PM4_HEADER.unpack_from(d, o)
            o += PM4_HEADER.size
            pm4[seq] = (first, struct.unpack_from(">%dI" % n, d, o))
            o += 4 * n
    return funcs, events, dumps, pm4


def decode_pm4(first, words):
    """[(address, text)] for the packets in `words`."""
    out = []
    i = 0
    while i < len(words):
        hdr = words[i]
        typ = hdr >> 30
        addr = first + 4 * i
        if typ == 3:
            count = ((hdr >> 16) & 0x3FFF) + 1
            op = (hdr >> 8) & 0x7F
            name = PM4_OPS.get(op, "op%02X" % op)
            body = words[i + 1:i + 1 + count]
            text = "%s(%d)" % (name, count)
            if op in (0x2D, 0x55) and body:
                kind = CONST_TYPES.get((body[0] >> 16) & 7, "?")
                text += " %s index %#x, %d value(s): %s" % (kind, body[0] & 0xFFFF, len(body) - 1, " ".join(h(x) for x in body[1:7]) + (" ..." if len(body) > 7 else ""))
            elif op == 0x27 and len(body) >= 2:
                text += " type %d addr %s size %d dwords" % (body[0] & 3, h(body[0] & ~3), body[1] & 0xFFFF)
            elif op in (0x22, 0x36):
                text += " " + " ".join(h(x) for x in body[:4])
            else:
                text += " " + " ".join(h(x) for x in (body if FULL else body[:6])) + (" ..." if len(body) > 6 and not FULL else "")
            out.append((addr, text))
            i += 1 + count
        elif typ == 0:
            count = ((hdr >> 16) & 0x3FFF) + 1
            reg = hdr & 0x7FFF
            out.append((addr, "REG_WRITE reg %#x x%d: %s" % (reg, count, " ".join(h(x) for x in words[i + 1:i + 1 + (count if FULL else min(count, 4))]))))
            i += 1 + count
        elif typ == 2:
            out.append((addr, "NOP2"))
            i += 1
        else:
            out.append((addr, "type1 %s %s" % (h(hdr), h(words[i + 1]) if i + 1 < len(words) else "")))
            i += 2
    return out


def texture_fetch(w):
    """GPUTEXTURE_FETCH_CONSTANT (6 dwords, already host order)."""
    return {
        "type": w[0] & 3, "pitch": ((w[0] >> 22) & 0x1FF) * 32, "tiled": (w[0] >> 31) & 1,
        "format": w[1] & 0x3F, "endian": (w[1] >> 6) & 3, "base": (w[1] >> 12) << 12,
        "width": (w[2] & 0x1FFF) + 1, "height": ((w[2] >> 13) & 0x1FFF) + 1,
        "dim": (w[5] >> 9) & 3, "mip_address": (w[5] >> 12) << 12, "mip_min": (w[4] >> 2) & 0xF, "mip_max": (w[4] >> 6) & 0xF,
    }


def window_frames(prefix, events, dumps):
    """Frames of the full-recording window: from the .counts file, or (older recordings) the frames whose events carry pointer dumps."""
    try:
        for line in open(prefix + ".counts"):
            if line.startswith("window "):
                _, first, n = line.split()
                return list(range(int(first), int(first) + int(n)))
    except OSError:
        pass
    return sorted({e["frame"] for e in events if e["seq"] in dumps and e["fn"] != FRAME_MARK})


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    prefix, cmd = sys.argv[1], sys.argv[2]
    funcs, events, dumps, pm4 = load(prefix)
    name = lambda f: "mark" if f == FRAME_MARK else "sub_%s" % h(funcs[f])
    marks = [e for e in events if e["fn"] == FRAME_MARK]
    if cmd == "counts":
        for line in open(prefix + ".counts"):
            print(line.rstrip())
        return
    if cmd == "swaps":
        for e in marks:
            d = dumps.get(e["seq"], [])
            print("frame %-5d thread %d  r3..r8 %s  ret %d" % (e["args"][0], e["thread"], " ".join(h(x) for x in e["args"][1:]), e["ret"]))
            for phase, arg, b in d:
                if arg == 1:  # the fetch constant pointer
                    w = struct.unpack(">16I", b)
                    print("      fetch constant at %s: %s" % (h(e["args"][1]), texture_fetch(w[:6])))
        return
    win = [e for e in events if e["fn"] != FRAME_MARK]
    if cmd == "functions":
        # the window frames are the ones with a full recording: far more events than the capped frames around them
        win_frames = window_frames(prefix, events, dumps)
        dense = set(win_frames)
        print("window frames: %s" % win_frames)
        calls, threads, words, ops = collections.Counter(), collections.defaultdict(set), collections.Counter(), collections.defaultdict(collections.Counter)
        pm4_by_exit = {}
        for e in win:
            if e["frame"] not in dense:
                continue
            if e["phase"] == 0:
                calls[e["fn"]] += 1
                threads[e["fn"]].add(e["thread"])
            elif e["phase"] == 1 and e["seq"] in pm4:
                first, ws = pm4[e["seq"]]
                words[e["fn"]] += len(ws)
                for _, text in decode_pm4(first, ws):
                    ops[e["fn"]][text.split("(")[0].split(" ")[0]] += 1
        print("%d functions called; calls are per window (%d frames)" % (len(calls), len(win_frames)))
        for f, n in sorted(calls.items(), key=lambda kv: funcs[kv[0]]):
            o = ", ".join("%s x%d" % kv for kv in ops[f].most_common(4))
            print("sub_%s  calls/frame %-7.1f threads %-3d pm4 words %-6d %s" % (h(funcs[f]), n / max(len(win_frames), 1), len(threads[f]), words[f], o))
        return
    if cmd == "sequence":
        dense = window_frames(prefix, events, dumps)
        frame = int(sys.argv[3]) if len(sys.argv) > 3 else dense[0]
        limit = int(sys.argv[4]) if len(sys.argv) > 4 else 80
        start = int(sys.argv[5].replace("0x", ""), 16) if len(sys.argv) > 5 else None
        skip = (int(sys.argv[6]) if len(sys.argv) > 6 else 1) - 1
        depth = collections.defaultdict(int)
        shown = 0
        for e in win:
            if e["frame"] != frame or shown >= limit:
                continue
            if start is not None:
                if e["phase"] == 0 and funcs[e["fn"]] == start:
                    if skip == 0:
                        start = None
                    else:
                        skip -= 1
                if start is not None:
                    continue
            t = e["thread"]
            if e["phase"] == 0:
                print("%s> sub_%s(%s) t%d" % ("  " * depth[t], h(funcs[e["fn"]]), ", ".join(h(x) for x in e["args"][:5]), t))
                depth[t] += 1
                shown += 1
            else:
                depth[t] = max(0, depth[t] - 1)
                extra = ""
                if e["seq"] in pm4:
                    first, ws = pm4[e["seq"]]
                    extra = "  PM4 %d dwords: %s" % (len(ws), "; ".join(text for _, text in decode_pm4(first, ws)[:3]))
                print("%s< sub_%s -> %s%s" % ("  " * depth[t], h(funcs[e["fn"]]), h(e["ret"]), extra))
        return
    if cmd == "draws":
        dense = set(window_frames(prefix, events, dumps))
        rows = collections.defaultdict(collections.Counter)
        for e in win:
            if e["frame"] not in dense or e["phase"] != 1 or e["seq"] not in pm4:
                continue
            first, ws = pm4[e["seq"]]
            i = 0
            while i < len(ws):
                hdr = ws[i]
                typ = hdr >> 30
                if typ == 3:
                    count = ((hdr >> 16) & 0x3FFF) + 1
                    op = (hdr >> 8) & 0x7F
                    if op in (0x22, 0x36):
                        body = ws[i + 1:i + 1 + count]
                        init = body[1] if op == 0x22 and len(body) > 1 else body[0] if body else 0
                        prim, src = init & 0x3F, (init >> 6) & 3
                        rows[funcs[e["fn"]]]["%s prim %d src %s idx32 %d" % ("DRAW_INDX" if op == 0x22 else "DRAW_INDX_2", prim, ("dma", "imm", "auto", "?")[src], (init >> 11) & 1)] += 1
                    i += 1 + count
                elif typ == 0:
                    i += 2 + ((hdr >> 16) & 0x3FFF)
                elif typ == 2:
                    i += 1
                else:
                    i += 2
        for f in sorted(rows):
            print("sub_%s" % h(f))
            for k, n in rows[f].most_common():
                print("    %-48s x%d" % (k, n))
        return
    if cmd == "toplevel":
        dense = window_frames(prefix, events, dumps)
        frame = int(sys.argv[3]) if len(sys.argv) > 3 else dense[0]
        only = int(sys.argv[4]) if len(sys.argv) > 4 else None
        depth = collections.defaultdict(int)
        inner = collections.defaultdict(int)       # calls nested in the current top-level call, per thread
        for e in win:
            if e["frame"] != frame:
                continue
            t = e["thread"]
            if e["phase"] == 0:
                if depth[t] == 0 and (only is None or t == only):
                    inner[t] = 0
                    print("t%d > sub_%s(%s)" % (t, h(funcs[e["fn"]]), ", ".join(h(x) for x in e["args"][:6])))
                else:
                    inner[t] += 1
                depth[t] += 1
            else:
                depth[t] = max(0, depth[t] - 1)
                if depth[t] == 0 and (only is None or t == only):
                    extra = ""
                    if e["seq"] in pm4:
                        first, ws = pm4[e["seq"]]
                        extra = "  PM4 %d dwords" % len(ws)
                    print("t%d < sub_%s -> %s  (%d nested calls)%s" % (t, h(funcs[e["fn"]]), h(e["ret"]), inner[t], extra))
        return
    if cmd == "function":
        want = int(sys.argv[3].replace("0x", ""), 16)
        limit = int(sys.argv[4]) if len(sys.argv) > 4 else 6
        shown = 0
        in_window = set(window_frames(prefix, events, dumps)) if "all" not in sys.argv[4:] else None
        for e in win:
            if funcs[e["fn"]] != want or (in_window is not None and e["frame"] not in in_window):
                continue
            if shown >= limit * 2:
                break
            shown += 1
            print("%d %s t%d frame %d args %s ret %s" % (e["seq"], "enter" if e["phase"] == 0 else "exit ", e["thread"], e["frame"], " ".join(h(x) for x in e["args"]), h(e["ret"])))
            for phase, arg, b in dumps.get(e["seq"], []):
                print("      r%d -> %s" % (3 + arg, " ".join(h(x) for x in struct.unpack(">16I", b))))
            if e["seq"] in pm4:
                first, ws = pm4[e["seq"]]
                for a, text in decode_pm4(first, ws):
                    print("      PM4 %s %s" % (h(a), text))
        return
    sys.exit(__doc__)


if __name__ == "__main__":
    main()
