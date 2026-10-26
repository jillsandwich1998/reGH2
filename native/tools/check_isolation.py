#!/usr/bin/env python3
"""The isolation rules of the native module, checked (see docs/abi_recomp_runtime.md, section 8).

  check_isolation.py [BUILD_DIR]

Without BUILD_DIR only the source rules are checked; with it (a build configured with GH2_ISOLATED_CPU=ON) the build is checked too.
  S1  native/rt/abi/include, native/rt/hooks, native/rt/trace, native/platform, native/kernel and native/gfx/{gpu,vk,render} do not use the SDK namespace (`rex::`) in code (comments
      excepted); native/platform and native/kernel do not include a `rex/` header either (rt/ may: <rex/ppc/...> there is the shim's own copy)
  S2  nothing under native/ carries a Xenia copyright header (the project writes or copies only ReXGlue/XenonRecomp-lineage files)
  S3  contract/imports.txt equals the imports of the generated code; the tracing wrappers are up to date; no vendored-header drift
  B1  the shim-only targets (gh2_cpu, gh2rt_hooks, gh2rt_trace) are compiled with no include directory that holds an SDK `rex/` tree
  B2  external_surface.py on libgh2_cpu.a and on the hooks/trace objects: no FORBIDDEN symbol, nothing outside the contract
Exit status 0 = all checks pass.
"""
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys

NATIVE = pathlib.Path(__file__).resolve().parents[1]
TOOLS = NATIVE / "tools"
WORK = NATIVE.parents[1]
SHIM = (NATIVE / "rt/abi/shim").resolve()
results = []


def report(name, ok, detail):
    results.append(ok)
    print("[%s] %-3s %s" % ("ok" if ok else "!!", name, detail))


def code_lines(path):
    in_block = False
    for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        s = line.strip()
        if in_block:
            if "*/" in s:
                in_block = False
            continue
        if s.startswith("/*"):
            in_block = "*/" not in s
            continue
        if s.startswith("//") or s.startswith("*"):
            continue
        yield n, re.sub(r"//.*$", "", line)


def s1():
    bad = []
    for sub in ("rt/abi/include", "rt/hooks", "rt/trace", "platform", "kernel", "gfx/gpu", "gfx/vk", "gfx/render"):
        for p in sorted((NATIVE / sub).rglob("*")):
            # platform/sdl3.cmake names the SDK's imported target rex::runtime on purpose (that is how it gets the SDK's SDL3): build glue, not code
            if p.suffix in (".h", ".cpp", ".hpp") or (p.suffix == ".cmake" and sub not in ("platform", "kernel", "gfx/gpu", "gfx/vk", "gfx/render")):
                for n, line in code_lines(p):
                    pattern = r"\brex::|#\s*include\s*[<\"]rex/" if sub in ("platform", "kernel", "gfx/gpu", "gfx/vk", "gfx/render") else r"\brex::"
                    if re.search(pattern, line):
                        bad.append("%s:%d" % (p.relative_to(NATIVE), n))
    report("S1", not bad, "no `rex::` and no rex/ include in the code of abi/include, hooks, trace, platform, kernel, gfx/gpu, gfx/vk, gfx/render" + ("" if not bad else ": " + ", ".join(bad[:6])))


def s2():
    bad = []
    for p in NATIVE.rglob("*"):
        if p.is_file() and p.suffix in (".h", ".cpp", ".py", ".sh", ".cmake", ".txt"):
            if re.search(r"Ben Vani[k]|Xenia : Xbox 360 Emulator Researc[h]", p.read_text(errors="replace")):
                bad.append(str(p.relative_to(NATIVE)))
    report("S2", not bad, "no Xenia copyright header under native/" + ("" if not bad else ": " + ", ".join(bad)))


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def s3():
    gen = WORK / "gh2_v10/generated/default"
    if (gen / "gh2_init.cpp").exists():
        r = run([sys.executable, str(TOOLS / "extract_imports.py"), str(gen), "--check", str(NATIVE / "rt/abi/contract/imports.txt")])
        report("S3a", r.returncode == 0, (r.stdout.strip() or r.stderr.strip()).splitlines()[-1])
    else:
        report("S3a", True, "generated code not found: imports.txt not compared (skipped)")
    r = run([sys.executable, str(TOOLS / "gen_import_tracer.py"), "--check"])
    report("S3b", r.returncode == 0, r.stdout.strip())
    r = run([sys.executable, str(TOOLS / "check_vendored_drift.py")])
    report("S3c", r.returncode == 0, r.stdout.strip().splitlines()[0])


def build_checks(build):
    build = pathlib.Path(build)
    r = run(["ninja", "-C", str(build), "-t", "compdb"])
    if r.returncode != 0:
        report("B1", False, "could not read the compilation database: " + r.stderr.strip()[:200])
        return
    shim_only = re.compile(r"native/CMakeFiles/(gh2_cpu|gh2rt_hooks|gh2rt_trace)\.dir/")
    seen, bad = 0, set()
    for e in json.loads(r.stdout):
        if not shim_only.search(e.get("output", "")):
            continue
        seen += 1
        args = shlex.split(e["command"])
        dirs = []
        for i, a in enumerate(args):
            if a in ("-I", "-isystem") and i + 1 < len(args):
                dirs.append(args[i + 1])
            elif a.startswith("-I") and len(a) > 2:
                dirs.append(a[2:])
        for d in dirs:
            dd = pathlib.Path(d)
            if not dd.is_absolute():
                dd = pathlib.Path(e["directory"]) / dd
            if (dd / "rex").exists() and dd.resolve() != SHIM:
                bad.add(str(dd))
    report("B1", seen > 0 and not bad, "%d shim-only translation units; include dirs holding an SDK rex/ tree: %s" % (seen, sorted(bad) or "none"))
    cmd = [sys.executable, str(TOOLS / "external_surface.py")]
    lib = build / "native/libgh2_cpu.a"
    objs = [str(build / "native/CMakeFiles/gh2rt_hooks.dir"), str(build / "native/CMakeFiles/gh2rt_trace.dir")]
    if lib.exists():
        r = run(cmd + [str(lib)])
        report("B2a", r.returncode == 0, "libgh2_cpu.a: " + " | ".join(l.strip() for l in r.stdout.splitlines()[:5] if "contract" in l or "FORBIDDEN" in l or "other" in l or "EXTERNAL" in l))
    else:
        report("B2a", True, "libgh2_cpu.a not built in this directory (GH2_ISOLATED_CPU=OFF): skipped")
    if all(pathlib.Path(o).exists() for o in objs):
        r = run(cmd + objs)
        out = r.stdout.splitlines()
        report("B2b", r.returncode == 0, "hooks + trace objects: " + " | ".join(l.strip() for l in out[:5] if "FORBIDDEN" in l or "other" in l or "EXTERNAL" in l))
    else:
        report("B2b", False, "hooks/trace objects not built")


def main():
    s1()
    s2()
    s3()
    if len(sys.argv) > 1:
        build_checks(sys.argv[1])
    ok = all(results)
    print("OVERALL: %s" % ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
