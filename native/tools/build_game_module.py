#!/usr/bin/env python3
"""Builds the game module: the recompiled game code alone, plus the module side of the handshake.
Windows: gh2game.dll with llvm-mingw. Linux (--os linux): libgh2game.so with the given clang (the AppImage's bundled one + its sysroot).

No Microsoft tool, header or library is involved: clang, lld, libc++ and the mingw-w64 headers and import libraries all come with
llvm-mingw. The DLL depends only on KERNEL32 and the Universal CRT that every Windows 10/11 has.

    build_game_module.py --generated <codegen dir> --native <repo>/native --simde <dir with simde/> --overrides overrides.txt
                         --toolchain <llvm-mingw root> --ninja ninja.exe --build <work dir> --out <dir>/gh2game.dll [-j N]

    build_game_module.py --os linux --cxx <clang++> [--cflags "<extra>"] [--ldflags "<extra>"] ... --out <dir>/libgh2game.so

Writes <work dir>/build.ninja and runs Ninja there; its output is the usual "[n/N] Building CXX object ..." progress (the launcher reads it).
The import list is native/rt/abi/contract/imports.txt; the override list comes from the host's build (gen_module_stubs.py overrides).
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys

TOOLS = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import gen_module_stubs  # noqa: E402

TARGET = "x86_64-w64-mingw32"
# Linux: the flags the generated code had when it was linked into the executable (gh2_cpu_flags + the Release flags), plus what a shared
# object needs; only gh2game_open is exported (GH2GAME_EXPORT) and the module binds its own symbols (-Bsymbolic). The small code model
# (like the Windows module): the module is far below 2 GB and its hidden functions call each other directly; -mcmodel=large was for the
# executable, whose image the SDK's link layout puts out of reach of 32-bit offsets.
LINUX_CXXFLAGS = ["-O2", "-DNDEBUG", "-std=gnu++23", "-gline-tables-only", "-msse4.1", "-fPIC", "-fvisibility=hidden"]
LINUX_LDFLAGS = ["-shared", "-fuse-ld=lld", "-Wl,-Bsymbolic", "-Wl,-z,defs", "-Wl,--gc-sections", "-Wl,-O1", "-Wl,--build-id"]
CXXFLAGS = ["--target=" + TARGET, "-O2", "-DNDEBUG", "-std=gnu++23", "-msse4.1", "-fno-exceptions", "-gcodeview", "-gline-tables-only",
            "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN", "-D_CRT_SECURE_NO_WARNINGS", "-D_CRT_NONSTDC_NO_WARNINGS"]


def ninja_path(p):
    """A path in a Ninja build statement."""
    return str(p).replace("\\", "/").replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def arg(a):
    """One argument of a command line (Ninja hands it to CreateProcess; clang splits it the MSVCRT way)."""
    a = str(a).replace("\\", "/")
    return '"%s"' % a if (" " in a or "\t" in a) else a


def generated_sources(gen_dir):
    text = (gen_dir / "sources.cmake").read_text()
    names = re.findall(r"\$\{CMAKE_CURRENT_LIST_DIR\}/([^\s)]+\.cpp)", text)
    names = [n for n in names if n != "gh2_register.cpp"]   # gh2_RegisterFunctions() has no caller (ReXApp uses PPCImageConfig.func_mappings)
    if not names:
        sys.exit("%s: no generated source listed" % (gen_dir / "sources.cmake"))
    return [gen_dir / n for n in names]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--generated", required=True, type=pathlib.Path)
    ap.add_argument("--native", required=True, type=pathlib.Path)
    ap.add_argument("--simde", required=True, type=pathlib.Path)
    ap.add_argument("--overrides", required=True, type=pathlib.Path)
    ap.add_argument("--os", choices=["windows", "linux"], default="windows" if os.name == "nt" else "linux")
    ap.add_argument("--toolchain", type=pathlib.Path, help="windows: the llvm-mingw root")
    ap.add_argument("--cxx", help="linux: the clang++ to use")
    ap.add_argument("--cflags", default="", help="linux: extra compile flags (--sysroot=..., --gcc-install-dir=...)")
    ap.add_argument("--ldflags", default="", help="linux: extra link flags")
    ap.add_argument("--ninja", required=True)
    ap.add_argument("--build", required=True, type=pathlib.Path)
    ap.add_argument("--out", required=True, type=pathlib.Path)
    ap.add_argument("-j", "--jobs", type=int, default=0)
    ap.add_argument("--configure-only", action="store_true", help="write build.ninja and the stubs, do not build")
    a = ap.parse_args()

    gen, native, build = a.generated.resolve(), a.native.resolve(), a.build.resolve()
    linux = a.os == "linux"
    if linux:
        clang = pathlib.Path(a.cxx or "clang++")
        flags = LINUX_CXXFLAGS + a.cflags.split()
    else:
        if not a.toolchain:
            sys.exit("--toolchain (llvm-mingw) is needed for the Windows module")
        clang = a.toolchain.resolve() / "bin" / "clang++.exe"
        if not clang.exists():
            sys.exit("llvm-mingw not found: %s" % clang)
        flags = CXXFLAGS
    if not (a.simde / "simde" / "x86" / "sse4.1.h").exists():
        sys.exit("simde not found under %s" % a.simde)
    build.mkdir(parents=True, exist_ok=True)

    imports = native / "rt/abi/contract/imports.txt"
    stubs = build / "gh2game_stubs.gen.cpp"
    gen_module_stubs.write_if_changed(stubs, gen_module_stubs.game_side(gen_module_stubs.read_list(imports), gen_module_stubs.read_list(a.overrides),
                                                                        (imports.name, a.overrides.name)))

    includes = ["-I" + str(gen), "-I" + str(native / "rt/abi/shim"), "-I" + str(native / "rt/abi/include"), "-I" + str(native / "rt/module/include"),
                "-isystem", str(a.simde.resolve())]
    base = " ".join(arg(x) for x in [clang] + flags + includes)
    out = a.out.resolve()
    pdb = out.with_suffix(".pdb")

    lines = ["# GENERATED by native/tools/build_game_module.py. Do not edit.",
             "ninja_required_version = 1.10",
             "rule pch",
             "  command = %s -x c++-header $in -o $out -MD -MF $out.d" % base,
             "  deps = gcc", "  depfile = $out.d", "  description = Building CXX precompiled header $out",
             "rule cxx_pch",
             "  command = %s -include-pch gh2_pch.h.pch -c $in -o $out -MD -MF $out.d" % base,
             "  deps = gcc", "  depfile = $out.d", "  description = Building CXX object $out",
             "rule cxx",
             "  command = %s -c $in -o $out -MD -MF $out.d" % base,
             "  deps = gcc", "  depfile = $out.d", "  description = Building CXX object $out",
             "rule link",
             ("  command = %s %s -o $out @$out.rsp" % (arg(clang), " ".join(arg(x) for x in LINUX_LDFLAGS + a.cflags.split() + a.ldflags.split())) if linux else
              "  command = %s --target=%s -shared -static -o $out @$out.rsp -Wl,--pdb=%s -Wl,--no-insert-timestamp" % (arg(clang), TARGET, arg(pdb))),
             "  rspfile = $out.rsp", "  rspfile_content = $in", "  description = Linking CXX shared library $out",
             "build gh2_pch.h.pch: pch %s" % ninja_path(gen / "gh2_pch.h")]
    objs = []
    for src in generated_sources(gen):
        obj = "obj/%s.o" % src.stem
        objs.append(obj)
        lines.append("build %s: cxx_pch %s | gh2_pch.h.pch" % (obj, ninja_path(src)))
    for src, obj in ((native / "rt/module/game_side.cpp", "obj/game_side.o"), (stubs, "obj/gh2game_stubs.gen.o")):
        objs.append(obj)
        lines.append("build %s: cxx %s" % (obj, ninja_path(src)))
    lines += ["build %s: link %s" % (ninja_path(out), " ".join(objs)), "default %s" % ninja_path(out), ""]
    gen_module_stubs.write_if_changed(build / "build.ninja", "\n".join(lines))
    if a.configure_only:
        return

    out.parent.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if k.upper() not in ("INCLUDE", "LIB", "LIBPATH", "CPATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH")}
    cmd = [a.ninja, "-C", str(build)] + (["-j", str(a.jobs)] if a.jobs > 0 else [])
    sys.exit(subprocess.call(cmd, env=env))


if __name__ == "__main__":
    main()
