#!/usr/bin/env python3
"""What do these objects/archives need from the outside? (the real contract, measured with nm)

  external_surface.py [--list] [--nm] FILE.a|FILE.o|DIR ...

external = undefined symbols of all inputs minus the symbols the inputs define themselves. Every external symbol is classified:
  contract   gh2abi::*, a kernel import (__imp__<Name> listed in native/rt/abi/contract/imports.txt, or its link-time-wrapped form
             __real___imp__<Name>), gh2_spin_hint, and the original guest functions the hooks wrap (__imp__sub_<ADDR>, listed in
             native/rt/hooks/hooked_functions.txt, native/gfx/guest_d3d/semantic_hooks.txt and the generated d3d_probe.gen.cpp)
  toolchain  exported by the C/C++ runtime (libc, libm, libstdc++, libgcc_s, ...) - not part of our contract
  FORBIDDEN  an SDK / Xenia / logging-library symbol (rex::, xe::, spdlog, fmt::) - the isolation is broken
  other      anything else - it must be added to the contract (or removed) before it is accepted
Exit status 1 when there is any FORBIDDEN or other symbol. --list prints every external symbol, --nm prints them like `nm -u`.
"""
import os
import pathlib
import re
import subprocess
import sys

NATIVE = pathlib.Path(__file__).resolve().parents[1]
LIBC_NONSHARED = {"atexit", "at_quick_exit", "pthread_atfork", "stat", "fstat", "lstat", "fstatat", "mknod", "mknodat"}
RUNTIME_LIBS = ["libc.so.6", "libm.so.6", "libstdc++.so.6", "libgcc_s.so.1", "libpthread.so.0", "libdl.so.2", "ld-linux-x86-64.so.2"]
FORBIDDEN = re.compile(r"(^|[^A-Za-z0-9_])(rex|xe)::|spdlog|fmt::|_ZN3rex|_ZN2xe")


def nm(args, files):
    out = subprocess.run(["nm"] + args + files, capture_output=True, text=True, check=True).stdout
    return out.splitlines()


def runtime_exports():
    names = set()
    dirs = ["/usr/lib", "/lib64", "/usr/lib64", "/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu"]
    for lib in RUNTIME_LIBS:
        for d in dirs:
            p = os.path.join(d, lib)
            if os.path.exists(p):
                for line in nm(["-D", "--defined-only"], [p]):
                    parts = line.split()
                    if len(parts) >= 3:
                        names.add(parts[2].split("@")[0])
                break
    return names


def demangle(names):
    if not names:
        return {}
    out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True, check=True).stdout.splitlines()
    return dict(zip(names, out))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    files = []
    for a in args:
        p = pathlib.Path(a)
        files += sorted(str(x) for x in p.rglob("*.o")) if p.is_dir() else [a]
    undefined, defined = set(), set()
    for line in nm(["-A", "-u"], files):
        parts = line.split()
        if parts and parts[-2:-1] in (["U"], ["w"], ["v"]):
            undefined.add(parts[-1])
    for line in nm(["--defined-only"], files):
        parts = line.split()
        if len(parts) >= 3 and re.fullmatch(r"[A-Za-z]", parts[-2]):
            defined.add(parts[-1])
    external = sorted(undefined - defined)
    dm = demangle(external)
    imports = set((NATIVE / "rt/abi/contract/imports.txt").read_text().split())
    hooked_fns = set((NATIVE / "rt/hooks/hooked_functions.txt").read_text().split())
    # the D3D library hooks (native/gfx/guest_d3d): the hand-written ones and the generated discovery probes wrap guest functions the same way
    semantic = NATIVE / "gfx/guest_d3d/semantic_hooks.txt"
    if semantic.exists():
        hooked_fns |= {l.split("#")[0].strip() for l in semantic.read_text().splitlines() if l.split("#")[0].strip()}
    probes = NATIVE / "gfx/guest_d3d/d3d_probe.gen.cpp"
    if probes.exists():
        hooked_fns |= {"sub_" + a for a in re.findall(r"GH2_D3D_PROBE_FN\(\d+, ([0-9A-F]{8})\)", probes.read_text())}
    rt = runtime_exports()
    cls = {"contract": [], "toolchain": [], "FORBIDDEN": [], "other": []}
    for s in external:
        d = dm[s]
        if FORBIDDEN.search(d) or FORBIDDEN.search(s):
            cls["FORBIDDEN"].append(s)
        elif (d.startswith(("gh2abi::", "gh2k::")) or s == "gh2_spin_hint" or (s.startswith("__imp__") and s[len("__imp__"):] in imports)
              or (s.startswith("__real___imp__") and s[len("__real___imp__"):] in imports)
              or (s.startswith("__imp__sub_") and s[len("__imp__"):] in hooked_fns)):
            cls["contract"].append(s)
        elif s in rt or s.split("@")[0] in rt or s in LIBC_NONSHARED or s in ("_GLOBAL_OFFSET_TABLE_", "__dso_handle", "_Unwind_Resume"):
            cls["toolchain"].append(s)
        else:
            cls["other"].append(s)
    imps = [s for s in cls["contract"] if s.startswith(("__imp__", "__real___imp__")) and not s.startswith("__imp__sub_")]
    gfn = [s for s in cls["contract"] if s.startswith("__imp__sub_")]
    abi = [s for s in cls["contract"] if s not in imps and s not in gfn]
    unused = sorted(imports - set(s.split("__imp__", 1)[1] for s in imps))
    print("inputs: %d file(s); undefined symbols (sum over members) %d, defined %d; EXTERNAL (undefined - defined) = %d" %
          (len(files), len(undefined), len(defined), len(external)))
    print("  contract : %3d  = %d kernel imports (of %d in the contract) + %d runtime services (gh2abi::*, gh2_spin_hint) + %d hooked guest functions" %
          (len(cls["contract"]), len(imps), len(imports), len(abi), len(gfn)))
    print("  toolchain: %3d  (libc / libm / libstdc++ / libgcc)" % len(cls["toolchain"]))
    print("  FORBIDDEN: %3d" % len(cls["FORBIDDEN"]))
    print("  other    : %3d" % len(cls["other"]))
    if unused:
        print("  imports in the contract that these inputs never reference: %d (%s)" % (len(unused), ", ".join(unused[:8]) + (" ..." if len(unused) > 8 else "")))
    print("runtime services required:")
    for s in abi:
        print("  U %s" % dm[s])
    for k in ("FORBIDDEN", "other"):
        for s in cls[k]:
            print("  %s: %s" % (k, dm[s]))
    if "--list" in sys.argv or "--nm" in sys.argv:
        print("toolchain symbols:")
        for s in cls["toolchain"]:
            print("  U %s" % s)
        print("kernel imports:")
        for s in imps:
            print("  U %s" % s)
    sys.exit(1 if cls["FORBIDDEN"] or cls["other"] else 0)


if __name__ == "__main__":
    main()
