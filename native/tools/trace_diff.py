#!/usr/bin/env python3
"""Compares boundary traces (native/rt/trace) of a candidate build against golden runs of the same scenarios.

  trace_diff.py GOLDEN_DIR GOLDEN_PREFIX CANDIDATE_DIR CANDIDATE_PREFIX [--scenarios title,autostart,normal] [--json OUT]

For every scenario it loads <PREFIX>_<scenario>_<n>.trace (+ .counts.json, .meta.json) of both sides and checks:
  1. SET      the set of kernel imports called is identical (golden runs of one scenario never differ in it);
  2. COUNTS   each import's exact call count lies in the golden band [min - m, max + m], m = max(10% of max, 2 x spread, 2);
  3. ORDER    the order in which imports are first seen (global, by sequence) matches the golden order as well as golden runs match
              each other (difflib ratio, 0.05 slack, never below 0.90);
  4. MAIN     the first 1500 events of the guest main thread (id, return value) match likewise;
  5. RETURNS  for the imports whose golden return values are always "clean" status codes (few distinct values, all of them 0, a
              small integer or an NTSTATUS / HRESULT code), the candidate returns no new failure status. Imports whose r3 on exit
              is a counter, a pointer or whatever a void function left there are not judged: in the golden runs NtClose, the
              spinlock/IRQL calls, RtlEnter/LeaveCriticalSection and others return such leftovers, different in every run.
Pseudo events (MMIO, indirect resolution; written only by the backend) are reported but never judged.
Exit status 0 = every check of every scenario passed.
"""
import difflib
import glob
import json
import os
import struct
import sys

EVENT = struct.Struct("<QIIHH6II")  # t_ns, seq, tid, id, flags, a[6], ret  (48 bytes, see boundary_trace.cpp)
MAIN_PREFIX = 1500


def load_run(path):
    meta = json.load(open(path + ".meta.json"))
    counts = json.load(open(path + ".counts.json"))
    names = meta["imports"]
    data = open(path, "rb").read()
    n = len(data) // EVENT.size
    events = [EVENT.unpack_from(data, i * EVENT.size) for i in range(n)]
    events.sort(key=lambda e: e[1])
    real = {names[i]: v for i, v in enumerate([0] * len(names))}  # placeholder to keep names order
    cnt = {k: v[0] for k, v in counts["counts"].items()}
    return {"names": names, "events": events, "counts": cnt, "path": path}


def first_seen(run):
    seen, order = set(), []
    for e in run["events"]:
        if e[4] & 2:  # pseudo
            continue
        name = run["names"][e[3]]
        if name not in seen:
            seen.add(name)
            order.append(name)
    return order


def main_prefix(run):
    real = [e for e in run["events"] if not (e[4] & 2)]
    if not real:
        return []
    tid = real[0][2]
    return [(run["names"][e[3]], e[10]) for e in real if e[2] == tid][:MAIN_PREFIX]


def ratio(a, b):
    return difflib.SequenceMatcher(None, a, b, autojunk=False).ratio()


def clean_status(v):
    """0, a small integer, or an NTSTATUS / HRESULT code (not -1 or 0x80000000, which counters and leftovers produce)."""
    return (v < 0x10000 or 0x40000000 <= v < 0x40010000 or 0x80000001 <= v < 0x80080000 or 0xC0000000 <= v < 0xC0100000)


def failure_like(v):
    return 0x80000001 <= v < 0x80080000 or 0xC0000000 <= v < 0xC0100000


def values_by_import(run):
    out = {}
    for e in run["events"]:
        if e[4] & 2:
            continue
        out.setdefault(run["names"][e[3]], set()).add(e[10])
    return out


def judge(scenario, golden, cand, ignore=frozenset()):
    res, ok = [], True
    pseudo = lambda n: n.startswith("<") or n in ignore   # --expect-diff names are judged by nobody: a documented consequence of a deliberate change
    gsets = [set(k for k in r["counts"] if not pseudo(k)) for r in golden]
    gunion = set().union(*gsets)
    gall = set.intersection(*gsets)
    csets = [set(k for k in r["counts"] if not pseudo(k)) for r in cand]
    # 1. SET
    bad = []
    for i, cs in enumerate(csets):
        if cs != gunion:
            bad.append("run %d: extra=%s missing=%s" % (i + 1, sorted(cs - gunion), sorted(gunion - cs)))
    res.append(("SET", not bad, "%d imports in golden (all runs agree: %s)%s" % (len(gunion), gunion == gall, "; " + "; ".join(bad) if bad else "")))
    # 2. COUNTS
    viol = []
    for name in sorted(gunion):
        vals = [r["counts"].get(name, 0) for r in golden]
        lo, hi = min(vals), max(vals)
        margin = max(0.10 * hi, 2 * (hi - lo), 2)
        for i, r in enumerate(cand):
            c = r["counts"].get(name, 0)
            if not (lo - margin <= c <= hi + margin):
                viol.append("%s run%d=%d golden[%d..%d]" % (name, i + 1, c, lo, hi))
    res.append(("COUNTS", not viol, "%d imports checked, %d outside the band%s" % (len(gunion), len(viol), ": " + "; ".join(viol[:8]) if viol else "")))
    # 3. ORDER
    gord = [first_seen(r) for r in golden]
    gpair = [ratio(gord[i], gord[j]) for i in range(len(gord)) for j in range(i + 1, len(gord))] or [1.0]
    cr = [min(ratio(first_seen(r), g) for g in gord) for r in cand]
    res.append(("ORDER", min(cr) >= max(0.90, min(gpair) - 0.05), "first-seen order ratio candidate>=%.3f vs golden-golden>=%.3f" % (min(cr), min(gpair))))
    # 4. MAIN
    gm = [main_prefix(r) for r in golden]
    mpair = [ratio(gm[i], gm[j]) for i in range(len(gm)) for j in range(i + 1, len(gm))] or [1.0]
    cm = [min(ratio(main_prefix(r), g) for g in gm) for r in cand]
    res.append(("MAIN", min(cm) >= max(0.90, min(mpair) - 0.05), "main-thread prefix (%d events) ratio candidate>=%.3f vs golden-golden>=%.3f" % (MAIN_PREFIX, min(cm), min(mpair))))
    # 5. RETURNS
    gvals = {}
    for r in golden:
        for k, v in values_by_import(r).items():
            gvals.setdefault(k, set()).update(v)
    judged = {k for k, v in gvals.items() if len(v) <= 12 and all(clean_status(x) for x in v)}
    newret = []
    for i, r in enumerate(cand):
        for k, v in values_by_import(r).items():
            if k in judged:
                bad = {x for x in v if failure_like(x)} - gvals[k]
                if bad:
                    newret.append("run%d %s new failure=%s" % (i + 1, k, [hex(x) for x in sorted(bad)]))
    res.append(("RETURNS", not newret, "%d of %d imports have clean status returns in golden and were judged%s" %
                (len(judged), len(gvals), "; " + "; ".join(newret[:6]) if newret else "")))
    ok = all(r[1] for r in res)
    pse = {}
    for r in cand:
        for k, v in r["counts"].items():
            if pseudo(k):
                pse.setdefault(k, []).append(v)
    return ok, res, pse


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = {a: sys.argv[sys.argv.index(a) + 1] for a in sys.argv if a in ("--scenarios", "--json", "--expect-diff")}
    if len(args) < 4:
        sys.exit(__doc__)
    gdir, gpre, cdir, cpre = args[:4]
    scenarios = opts.get("--scenarios", "title,autostart,normal").split(",")
    overall, report = True, {}
    for sc in scenarios:
        gfiles = sorted(f for f in glob.glob(os.path.join(gdir, "%s_%s_*.trace" % (gpre, sc))))
        cfiles = sorted(f for f in glob.glob(os.path.join(cdir, "%s_%s_*.trace" % (cpre, sc))))
        if not gfiles or not cfiles:
            print("== %s: no runs (golden %d, candidate %d)" % (sc, len(gfiles), len(cfiles)))
            overall = False
            continue
        golden, cand = [load_run(f) for f in gfiles], [load_run(f) for f in cfiles]
        ignore = frozenset(x for x in opts.get("--expect-diff", "").split(",") if x)
        ok, res, pse = judge(sc, golden, cand, ignore)
        overall &= ok
        print("== %s: golden %d runs, candidate %d runs -> %s" % (sc, len(golden), len(cand), "PASS" if ok else "FAIL"))
        for name, passed, text in res:
            print("   [%s] %-7s %s" % ("ok" if passed else "!!", name, text))
        if ignore:
            print("   (info) not judged (--expect-diff): %s" % ", ".join(sorted(ignore)))
        if pse:
            print("   (info) pseudo events in candidate: %s" % ", ".join("%s=%s" % (k, v) for k, v in sorted(pse.items())))
        report[sc] = {"pass": ok, "checks": {n: {"pass": p, "detail": t} for n, p, t in res}}
    print("OVERALL: %s" % ("PASS" if overall else "FAIL"))
    if "--json" in opts:
        json.dump({"overall": overall, "scenarios": report}, open(opts["--json"], "w"), indent=1)
    sys.exit(0 if overall else 1)


if __name__ == "__main__":
    main()
