#!/bin/sh
# Builds the patched ReXGlue SDK (pinned commit + sdk-patches/*.patch) and installs it into <prefix>/linux-amd64.
# usage: scripts/build-sdk.sh <install-prefix> [workdir]     (CC / CXX select the compiler, default clang / clang++)
set -eu
here="$(cd "$(dirname "$0")/.." && pwd)"
prefix="$(mkdir -p "$1" && cd "$1" && pwd)"
work="${2:-${TMPDIR:-/tmp}/gh2-rexsdk}"
base="$(cat "$here/sdk-patches/SDK_BASE_COMMIT")"
mkdir -p "$work"
if [ ! -d "$work/src/.git" ]; then git clone https://github.com/rexglue/rexglue-sdk "$work/src"; fi
cd "$work/src"
git fetch -q origin "$base" 2>/dev/null || git fetch -q origin
git reset -q --hard "$base"
git submodule update --init --depth 1
for p in "$here"/sdk-patches/*.patch; do
  echo "applying $(basename "$p")"
  git apply "$p"
done
cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
  -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_STANDARD=23 \
  -DREXGLUE_ENABLE_TRACY=OFF -DCMAKE_INSTALL_PREFIX="$prefix/linux-amd64"
cmake --build out
cmake --install out
echo "SDK installed in $prefix/linux-amd64"
