#!/bin/bash
# build-shader-tools.sh OUT_DIR - makes the folder the game needs to build its shader cache on the player's machine (first start):
#   OUT_DIR/native/tools/{shader_pipeline,shader_cache_build,ark_extract}.py
#   OUT_DIR/native/third_party/patches/*.patch                                (only read for the report's provenance)
#   OUT_DIR/native/third_party/XenosRecomp/XenosRecomp/shader_common.h        (the header XenosRecomp is given)
#   OUT_DIR/native/third_party/build/xenosrecomp/XenosRecomp/XenosRecomp      the patched XenosRecomp
#   ... next to it: libdxcompiler.so, libdxil.so, libfmt.so.11 and (with --runtime-libs DIR) libstdc++.so.6 / libgcc_s.so.1
# Runs in the builder container (Ubuntu 22.04: glibc 2.35 is the floor of the result) or on any machine with git, cmake, ninja and clang++.
set -euo pipefail
out="$1"; shift
runtime_libs=""
[ "${1:-}" = "--runtime-libs" ] && runtime_libs="$2"
here="$(cd "$(dirname "$0")/.." && pwd)"
native="$here/native"
rm -rf "$out"; mkdir -p "$out/native/tools" "$out/native/third_party/patches" "$out/native/third_party/XenosRecomp/XenosRecomp"
sh "$native/tools/build_xenosrecomp.sh" "${GH2_XENOS_BUILD:-$native/third_party/build/xenosrecomp}" >/dev/null
bin_src="${GH2_XENOS_BUILD:-$native/third_party/build/xenosrecomp}"
bin_dst="$out/native/third_party/build/xenosrecomp/XenosRecomp"
mkdir -p "$bin_dst"
cp "$bin_src/XenosRecomp/XenosRecomp" "$bin_dst/"
cp -aL "$native/third_party/XenosRecomp/thirdparty/dxc-bin/lib/x64/libdxcompiler.so" "$native/third_party/XenosRecomp/thirdparty/dxc-bin/lib/x64/libdxil.so" "$bin_dst/"
# whatever else the binary loads from the build tree ({fmt} here, when it is built as a shared library)
ldd "$bin_src/XenosRecomp/XenosRecomp" | awk '/=> \//{print $3}' | grep -E "^($bin_src|$native/third_party)" | while read -r so; do cp -aL "$so" "$bin_dst/"; done || true
if [ -n "$runtime_libs" ]; then cp -aL "$runtime_libs/libstdc++.so.6" "$runtime_libs/libgcc_s.so.1" "$bin_dst/"; fi
cp "$native/third_party/XenosRecomp/XenosRecomp/shader_common.h" "$out/native/third_party/XenosRecomp/XenosRecomp/"
cp "$native"/third_party/patches/*.patch "$out/native/third_party/patches/"
cp "$native/tools/shader_pipeline.py" "$native/tools/shader_cache_build.py" "$native/tools/ark_extract.py" "$out/native/tools/"
cp "$native/third_party/XenosRecomp/LICENSE"* "$out/native/third_party/XenosRecomp/" 2>/dev/null || true
du -sh "$out"
