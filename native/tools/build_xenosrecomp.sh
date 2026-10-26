#!/bin/sh
# build_xenosrecomp.sh [BUILD_DIR] - builds the patched XenosRecomp (native/third_party/XenosRecomp, see third_party/fetch.sh).
# Default build dir: native/third_party/build/xenosrecomp (ignored by git). Prints the path of the executable.
# Needs: cmake, ninja, a C++17 compiler (clang++ preferred). DXC comes with the sources (thirdparty/dxc-bin: libdxcompiler.so).
set -e
here="$(cd "$(dirname "$0")/.." && pwd)"
src="$here/third_party/XenosRecomp"
build="${1:-$here/third_party/build/xenosrecomp}"
[ -d "$src/XenosRecomp" ] || sh "$here/third_party/fetch.sh"
cxx="${GH2_CXX:-$(command -v clang++ || command -v c++)}"
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$cxx" >/dev/null
cmake --build "$build" -j"$(nproc)" >/dev/null
echo "$build/XenosRecomp/XenosRecomp"
