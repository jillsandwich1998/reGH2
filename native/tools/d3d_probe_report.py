#!/usr/bin/env python3
"""Reads what the D3D discovery probe recorded (GH2_D3D_PROBE=<prefix>, see native/gfx/guest_d3d/include/gh2gfx/d3d_probe.h).

  d3d_probe_report.py PREFIX summary                one line per function: calls, threads, how often an argument looked like a pointer
  d3d_probe_report.py PREFIX containers              functions that received a pointer to a shader container (first word 0x102A11xx)
  d3d_probe_report.py PREFIX follow ADDR [N]         the first N calls of one function with its arguments, dereferenced words and result
  d3d_probe_report.py PREFIX object VALUE            every call (any function) in which VALUE appears as an argument, a result or a dereferenced word
  d3d_probe_report.py PREFIX created ADDR ARG        for the function ADDR: the value the out-parameter ARG (0-based) holds at exit, i.e. the objects it creates
Record layout: native/gfx/guest_d3d/include/gh2gfx/d3d_probe.h (ProbeEvent, 64 bytes).
"""
import collections
import pathlib
import struct
import sys

EVENT = struct.Struct("<IHBB6II6II")   # seq, function, phase, thread, args[6], ret, deref[6], reserved


def load(prefix):
    funcs = {}
    for line in open(prefix + ".functions"):
        i, a = line.split()
        funcs[int(i)] = int(a, 16)
    data = pathlib.Path(prefix + ".events").read_bytes()
    events = [EVENT.unpack_from(data, o) for o in range(0, len(data) - EVENT.size + 1, EVENT.size)]
    ev = []
    for e in events:
        ev.append({"seq": e[0], "fn": e[1], "phase": e[2], "thread": e[3], "args": e[4:10], "ret": e[10], "deref": e[11:17]})
    return funcs, ev


def h(v):
    return "%08X" % v


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    prefix, cmd = sys.argv[1], sys.argv[2]
    funcs, ev = load(prefix)
    if cmd == "summary":
        calls, threads, ptrs = collections.Counter(), collections.defaultdict(set), collections.Counter()
        for e in ev:
            if e["phase"] == 0:
                calls[e["fn"]] += 1
                threads[e["fn"]].add(e["thread"])
                ptrs[e["fn"]] += sum(1 for d in e["deref"] if d != 0xFFFFFFFF)
        print("%d events, %d functions called" % (len(ev), len(calls)))
        for f, n in sorted(calls.items(), key=lambda kv: -kv[1])[:60]:
            print("sub_%s  calls>=%-5d threads=%d  pointer-args=%d" % (h(funcs[f]), n, len(threads[f]), ptrs[f]))
    elif cmd == "containers":
        found = collections.defaultdict(lambda: collections.Counter())
        sample = {}
        for e in ev:
            if e["phase"] != 0:
                continue
            for i, d in enumerate(e["deref"]):
                if d != 0xFFFFFFFF and (d & 0xFFFFFF00) == 0x102A1100:
                    found[e["fn"]][i] += 1
                    sample.setdefault((e["fn"], i), (e["args"][i], d, e["args"]))
        for f, c in sorted(found.items(), key=lambda kv: funcs[kv[0]]):
            for i, n in sorted(c.items()):
                a, d, args = sample[(f, i)]
                print("sub_%s: argument r%d points to a container in %d recorded call(s); e.g. container at %s (word %s), args %s" %
                      (h(funcs[f]), 3 + i, n, h(a), h(d), " ".join(h(x) for x in args)))
        if not found:
            print("no recorded argument pointed to a word 0x102A11xx")
    elif cmd == "follow":
        want = int(sys.argv[3].replace("0x", ""), 16)
        limit = int(sys.argv[4]) if len(sys.argv) > 4 else 12
        shown = 0
        for e in ev:
            if funcs[e["fn"]] == want and shown < limit * 2:
                shown += 1
                print("%6d %s t%d args %s  ret %s  deref %s" % (e["seq"], "enter" if e["phase"] == 0 else "exit ", e["thread"], " ".join(h(x) for x in e["args"]),
                                                              h(e["ret"]), " ".join(h(x) for x in e["deref"])))
    elif cmd == "object":
        v = int(sys.argv[3].replace("0x", ""), 16)
        for e in ev:
            hit = [("arg r%d" % (3 + i)) for i, a in enumerate(e["args"]) if a == v]
            hit += ["ret"] if e["phase"] == 1 and e["ret"] == v else []
            hit += [("deref r%d" % (3 + i)) for i, d in enumerate(e["deref"]) if d == v]
            if hit:
                print("%6d %s sub_%s t%d %s | args %s ret %s" % (e["seq"], "enter" if e["phase"] == 0 else "exit ", h(funcs[e["fn"]]), e["thread"], ",".join(hit),
                                                              " ".join(h(x) for x in e["args"]), h(e["ret"])))
    elif cmd == "created":
        want = int(sys.argv[3].replace("0x", ""), 16)
        arg = int(sys.argv[4])
        vals = []
        for e in ev:
            if funcs[e["fn"]] == want and e["phase"] == 1 and e["deref"][arg] != 0xFFFFFFFF:
                vals.append(e["deref"][arg])
        print("sub_%s: out-parameter r%d holds, at exit: %d values, %d distinct; first: %s" % (h(want), 3 + arg, len(vals), len(set(vals)), " ".join(h(x) for x in vals[:10])))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
