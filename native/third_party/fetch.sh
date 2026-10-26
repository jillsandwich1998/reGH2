#!/bin/sh
# fetch.sh - clones the third-party sources the native pipeline builds, at pinned commits. Their contents are NOT committed to
# this repository (see .gitignore); run this once, then build with native/tools/build_xenosrecomp.sh.
set -e
here="$(cd "$(dirname "$0")" && pwd)"

# XenosRecomp (hedge-dev, MIT): Xbox 360 shader microcode -> HLSL / SPIR-V. Submodules: smol-v, zstd, xxHash, fmt, dxc-bin.
XENOSRECOMP_URL=https://github.com/hedge-dev/XenosRecomp.git
XENOSRECOMP_COMMIT=990d03b28a27b50277ee5d8d942e1c5f873869d1

if [ ! -d "$here/XenosRecomp/.git" ]; then
  git clone "$XENOSRECOMP_URL" "$here/XenosRecomp"
fi
git -C "$here/XenosRecomp" fetch --quiet origin
git -C "$here/XenosRecomp" checkout --quiet "$XENOSRECOMP_COMMIT"
git -C "$here/XenosRecomp" submodule update --init --recursive --depth 1
# Our patches (third_party/patches/*.patch, independent of each other): applied once, detected when already applied.
for p in "$here"/patches/*.patch; do
  [ -e "$p" ] || continue
  if git -C "$here/XenosRecomp" apply --check "$p" 2>/dev/null; then
    git -C "$here/XenosRecomp" apply "$p"
    echo "applied $(basename "$p")"
  elif git -C "$here/XenosRecomp" apply --reverse --check "$p" 2>/dev/null; then
    echo "already applied $(basename "$p")"
  else
    echo "PATCH DOES NOT APPLY: $p" >&2
    exit 1
  fi
done
echo "XenosRecomp at $(git -C "$here/XenosRecomp" rev-parse --short HEAD) + patches"
