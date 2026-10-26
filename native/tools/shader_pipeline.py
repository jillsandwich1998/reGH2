#!/usr/bin/env python3
"""Shader pipeline S.0 - S.5, run on the player's machine (the shaders are the player's own, nothing here is committed).

  shader_pipeline.py --gen-dir GAME/gen --out WORK_DIR [--pe DEFAULT.PE] [--stages extract,xex,recompile,validate,cache,selftest] [--jobs N]
                     [--xenosrecomp BIN] [--target-env vulkan1.1] [--spirv-val BIN] [--cache-tool BIN]

  S.0 extract    the .fx_xbox files from gen/main.hdr + main_N.ark (native/tools/ark_extract.py)      -> WORK/fx_xbox/*.fx_xbox
  S.0b xex       containers embedded in the game's executable (the decoded PE image, --pe): the video player's shaders live there, not in the
                 ARK; found by the same header scan, written as xex_<address>.fx_xbox next to the others            -> WORK/fx_xbox/
  S.1/S.2        XenosRecomp (patched, third_party/patches) finds every shader container, hashes it (XXH3-64) and translates it to
      recompile  HLSL; DXC compiles it to SPIR-V                                                      -> WORK/emit/<hash>.hlsl|.spv, index.tsv
  S.4 validate   spirv-val on every module                                                            -> WORK/validate.json
  S.5 cache      shaders.pak + manifest.json + index.tsv (native/gfx/shader_cache)                    -> WORK/cache/
      selftest   gh2_shader_cache_tool: the C++ reader agrees with the writer; every extracted file is found by the run-time lookup;
                 unknown containers are logged once                                                   -> printed
  (S.3, the 1D-texture fix, is a patch of XenosRecomp: native/third_party/patches/0001-*.patch.)
The report (WORK/report.json and the last lines printed) gives the success rate of every step. Exit status 1 when a step lost shaders.
"""
import argparse
import concurrent.futures as cf
import csv
import hashlib
import json
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

HERE = pathlib.Path(__file__).resolve().parent
NATIVE = HERE.parent
sys.path.insert(0, str(HERE))
import ark_extract  # noqa: E402
import shader_cache_build  # noqa: E402


def sh(cmd, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, **kw)
    except FileNotFoundError as e:   # a tool a player's machine may not have (git, which, spirv-val): the caller sees a failed run, not a crash
        return subprocess.CompletedProcess(cmd, 127, "", str(e))


def pct(a, b):
    return "%.1f%%" % (100.0 * a / b) if b else "n/a"


def stage_extract(a, work, rep):
    t0 = time.time()
    hdr = ark_extract.locate(a.gen_dir)
    raw = hdr.read_bytes()
    version, encrypted, sizes, entries = ark_extract.parse_header(raw)
    fx = [e for e in entries if e.path.endswith(".fx_xbox")]
    out = work / "fx_xbox"
    out.mkdir(parents=True, exist_ok=True)
    names = set()
    for e in fx:
        name = pathlib.PurePosixPath(e.path).name
        if name in names:
            raise SystemExit("two ARK entries share the file name %s" % name)
        names.add(name)
        (out / name).write_bytes(ark_extract.read_entry(hdr, e))
    rep["extract"] = {"ark_version": version, "ark_header_encrypted": encrypted, "ark_entries": len(entries), "fx_xbox_files": len(fx),
                      "ark_hdr_sha1": hashlib.sha1(raw).hexdigest(), "seconds": round(time.time() - t0, 1)}
    print("S.0 extract   : %d .fx_xbox files from %s (ARK v%d, %d entries, %.1fs)" % (len(fx), hdr.name, version, len(entries), time.time() - t0))


def scan_containers(data, base_address=0x82000000):
    """Shader containers inside a memory image: header 0x102A11xx, zero words at +28 and +32, sizes inside the image (what XenosRecomp scans for)."""
    be32 = lambda o: int.from_bytes(data[o:o + 4], "big")
    found = []
    for off in range(0, len(data) - 36, 4):
        if (be32(off) & 0xFFFFFF00) != 0x102A1100:
            continue
        virtual_size, physical_size, shader_offset = be32(off + 4), be32(off + 8), be32(off + 24)
        if be32(off + 28) or be32(off + 32) or virtual_size > 1 << 20 or physical_size > 1 << 20 or off + virtual_size + physical_size > len(data):
            continue
        if shader_offset + 8 > virtual_size:
            continue
        found.append((base_address + off, data[off:off + virtual_size + physical_size]))
    return found


def stage_xex(a, work, rep):
    if not a.pe:
        print("S.0b xex      : no --pe given: SKIPPED (the video player's 3 shaders are missing from the cache without it)")
        rep["xex"] = {"skipped": True}
        return
    t0 = time.time()
    out = work / "fx_xbox"
    out.mkdir(parents=True, exist_ok=True)
    found = scan_containers(pathlib.Path(a.pe).read_bytes())
    for addr, blob in found:
        (out / ("xex_%08X.fx_xbox" % addr)).write_bytes(blob)
    rep["xex"] = {"containers": len(found), "addresses": ["%08X" % addr for addr, _ in found], "seconds": round(time.time() - t0, 1)}
    print("S.0b xex      : %d containers embedded in the executable (%s)" % (len(found), ", ".join("%08X" % addr for addr, _ in found)))


def find_xenosrecomp(a):
    if a.xenosrecomp:
        return a.xenosrecomp
    cand = NATIVE / "third_party/build/xenosrecomp/XenosRecomp/XenosRecomp"
    if cand.exists():
        return str(cand)
    if a.no_build:   # run by the game on its first start: building the tool needs the network and a compiler, so say what is missing instead
        print("ERROR: the XenosRecomp binary is not at %s (this run may not build it)" % cand, flush=True)
        sys.exit(3)
    return sh(["sh", str(HERE / "build_xenosrecomp.sh")]).stdout.strip().splitlines()[-1]


def stage_recompile(a, work, rep):
    t0 = time.time()
    xr = find_xenosrecomp(a)
    common = NATIVE / "third_party/XenosRecomp/XenosRecomp/shader_common.h"
    emit = work / "emit"
    # the shipped binary keeps libdxcompiler.so, {fmt} and the C++ runtime it was built with next to itself: they come first (a binary built on the dev machine finds them by its RUNPATH instead)
    env = dict(os.environ)
    own_libs = os.path.dirname(os.path.abspath(xr))
    env["LD_LIBRARY_PATH"] = own_libs + (":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
    r = sh([xr, str(work / "fx_xbox"), "-", str(common), "--emit", str(emit)], env=env)
    if r.returncode != 0 or not (emit / "index.tsv").exists():
        raise SystemExit("XenosRecomp failed (exit %d): %s" % (r.returncode, (r.stderr or r.stdout)[-400:]))
    rows = list(csv.DictReader(open(emit / "index.tsv"), delimiter="\t"))
    files = len(list((work / "fx_xbox").glob("*.fx_xbox")))  # the ARK's files plus the containers found in the executable
    ok = [x for x in rows if x["status"] == "ok"]
    files_ok = sum(int(x["occurrences"]) for x in ok)
    errors = {}
    for x in rows:
        if x["status"] != "ok":
            errors[x["error"][:90]] = errors.get(x["error"][:90], 0) + 1
    patches = sorted((NATIVE / "third_party/patches").glob("*.patch"))
    prov = {
        "xenosrecomp_binary": os.path.basename(os.path.dirname(os.path.dirname(xr))),
        "xenosrecomp_commit": sh(["git", "-C", str(NATIVE / "third_party/XenosRecomp"), "rev-parse", "HEAD"]).stdout.strip(),
        "patches_sha1": hashlib.sha1(b"".join(p.read_bytes() for p in patches)).hexdigest(),
        "patches": [p.name for p in patches],
    }
    rep["recompile"] = {"files": files, "containers": len(rows), "vs": sum(1 for x in rows if x["stage"] == "vs"),
                        "ps": sum(1 for x in rows if x["stage"] == "ps"), "hlsl_ok": len(rows), "dxc_ok": len(ok),
                        "files_covered_by_ok": files_ok, "dxc_errors": errors, "seconds": round(time.time() - t0, 1), "tool": prov}
    print("S.1/S.2       : %d files -> %d distinct containers (%d vertex, %d pixel); HLSL %d/%d; DXC -> SPIR-V %d/%d (%s of containers, "
          "%d/%d = %s of files) in %.1fs" % (files, len(rows), rep["recompile"]["vs"], rep["recompile"]["ps"], len(rows), len(rows),
                                             len(ok), len(rows), pct(len(ok), len(rows)), files_ok, files, pct(files_ok, files),
                                             time.time() - t0))
    for msg, n in errors.items():
        print("                 rejected by DXC (%d): %s" % (n, msg))


def stage_validate(a, work, rep):
    t0 = time.time()
    emit = work / "emit"
    rows = [x for x in csv.DictReader(open(emit / "index.tsv"), delimiter="\t") if x["status"] == "ok"]
    val = a.spirv_val or "spirv-val"
    version = sh([val, "--version"]).stdout.splitlines()[0] if shutil.which(val) else "missing"
    if version == "missing":
        print("S.4 validate  : spirv-val not found: SKIPPED")
        rep["validate"] = {"skipped": True}
        return

    def one(x):
        r = sh([val, "--target-env", a.target_env, str(emit / (x["hash"] + ".spv"))])
        return x["hash"], r.returncode, (r.stderr or r.stdout).strip().splitlines()[:1]

    with cf.ThreadPoolExecutor(a.jobs) as ex:
        res = list(ex.map(one, rows))
    bad = {h: msg for h, rc, msg in res if rc != 0}
    rep["validate"] = {"tool": version, "target_env": a.target_env, "checked": len(res), "valid": len(res) - len(bad), "invalid": bad,
                       "seconds": round(time.time() - t0, 1)}
    (work / "validate.json").write_text(json.dumps(rep["validate"], indent=1))
    print("S.4 validate  : spirv-val (%s, %s): %d/%d valid (%s) in %.1fs" % (version.split(" ")[0] + " " + version.split(" ")[1] if " " in version else version,
                                                                             a.target_env, len(res) - len(bad), len(res),
                                                                             pct(len(res) - len(bad), len(res)), time.time() - t0))


def stage_cache(a, work, rep):
    prov = {"source": rep.get("extract", {}), "tools": rep.get("recompile", {}).get("tool", {}),
            "dxc": sh(["sh", "-c", "LD_LIBRARY_PATH=%s %s --version" % (NATIVE / "third_party/XenosRecomp/thirdparty/dxc-bin/lib/x64",
                                                                       NATIVE / "third_party/XenosRecomp/thirdparty/dxc-bin/bin/x64/dxc-linux")]).stdout.strip(),
            "spirv_val": rep.get("validate", {}).get("tool", ""), "target_env": a.target_env}
    excluded = set(rep.get("validate", {}).get("invalid", {}))
    m = shader_cache_build.build(work / "emit", work / "cache", prov, excluded)
    s = m["stats"]
    rep["cache"] = {k: s[k] for k in ("containers_in_index", "shaders_in_pak", "vertex", "pixel", "spirv_bytes", "pak_bytes", "pak_sha1")}
    print("S.5 cache     : %d shaders in shaders.pak (%d vertex, %d pixel), %.2f MB; %d left out -> %s" %
          (s["shaders_in_pak"], s["vertex"], s["pixel"], s["pak_bytes"] / 1e6, len(s["left_out"]), work / "cache"))


def stage_selftest(a, work, rep):
    tool = a.cache_tool or str(NATIVE / "third_party/build/shader_cache/gh2_shader_cache_tool")
    if not os.path.exists(tool):
        build = NATIVE / "third_party/build/shader_cache"
        r = sh(["cmake", "-S", str(NATIVE / "gfx/shader_cache"), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"])
        r = sh(["cmake", "--build", str(build), "-j8"]) if r.returncode == 0 else r
        if r.returncode != 0:
            print("selftest      : could not build gh2_shader_cache_tool:\n" + (r.stderr or r.stdout)[-600:])
            rep["selftest"] = {"built": False}
            return
    v = sh([tool, "verify", str(work / "cache")])
    s = sh([tool, "selftest", str(work / "cache"), str(work / "cache/index.tsv"), str(work / "fx_xbox")])
    print("selftest      : " + v.stdout.strip().splitlines()[-1])
    for line in s.stdout.strip().splitlines():
        print("                " + line)
    rep["selftest"] = {"verify_rc": v.returncode, "selftest_rc": s.returncode}


def die_with_parent():
    """GH2_PARENT_PID (set by the game's first-run build): when that process is gone, this pipeline and everything it started goes too (the game starts it as the leader of a process
    group of its own, so the group is only ours). Closing the game in the middle of the build must not leave XenosRecomp running."""
    parent = int(os.environ.get("GH2_PARENT_PID", "0") or 0)
    if not parent:
        return

    def watch():
        while True:
            time.sleep(0.5)
            if os.getppid() != parent:
                if os.getpgrp() == os.getpid():
                    os.killpg(os.getpgrp(), signal.SIGKILL)
                os._exit(1)

    threading.Thread(target=watch, daemon=True).start()


def main():
    die_with_parent()
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--gen-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--pe", help="the decoded PE image of the game (default.pe) to scan for embedded shader containers")
    ap.add_argument("--stages", default="extract,xex,recompile,validate,cache,selftest")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--xenosrecomp")
    ap.add_argument("--target-env", default="vulkan1.1")
    ap.add_argument("--spirv-val")
    ap.add_argument("--cache-tool")
    ap.add_argument("--no-build", action="store_true", help="never build XenosRecomp (the game's automatic first-run build)")
    a = ap.parse_args()
    work = pathlib.Path(a.out).resolve()
    work.mkdir(parents=True, exist_ok=True)
    rep_path = work / "report.json"
    rep = json.loads(rep_path.read_text()) if rep_path.exists() else {}
    stages = a.stages.split(",")
    t0 = time.time()
    for name, fn in (("extract", stage_extract), ("xex", stage_xex), ("recompile", stage_recompile), ("validate", stage_validate),
                     ("cache", stage_cache), ("selftest", stage_selftest)):
        if name in stages:
            print("STAGE %s" % name, flush=True)   # the game's progress window follows these lines
            fn(a, work, rep)
            rep_path.write_text(json.dumps(rep, indent=1) + "\n")
    lost = rep.get("recompile", {}).get("containers", 0) - rep.get("cache", {}).get("shaders_in_pak", 0)
    print("total %.1fs; report: %s" % (time.time() - t0, rep_path))
    sys.exit(1 if "cache" in stages and lost else 0)


if __name__ == "__main__":
    main()
