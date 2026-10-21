#!/bin/bash
# Assembles the AppImage inside the builder container (Ubuntu 22.04, glibc 2.35).
# usage (container): make-appimage.sh /repo /out      where /out/sdk/linux-amd64 holds the patched SDK (scripts/build-sdk.sh)
# The AppImage carries: launcher (portable Python + Tk), the patched SDK, a hermetic C++ toolchain (clang 20 + lld + glibc 2.35
# and libstdc++ 13 sysroot, CMake, Ninja) and our source. Nothing of the game: the user's own copy is compiled on their machine.
set -euo pipefail
repo="$1"; out="$2"
app="$out/AppDir"
rm -rf "$app" && mkdir -p "$app/usr"/{bin,lib,python,sdk,toolchain/bin,sysroot,app}

echo "== SDK"
cp -a "$out/sdk/linux-amd64" "$app/usr/sdk/"

echo "== our sources"
for d in scripts launcher app sdk-patches; do cp -a "$repo/$d" "$app/usr/app/"; done
find "$app/usr/app" -name __pycache__ -prune -exec rm -rf {} +

echo "== clang 20 + lld"
llvm=/usr/lib/llvm-20
cp -aL "$llvm/bin/clang-20" "$app/usr/toolchain/bin/clang"
ln -sf clang "$app/usr/toolchain/bin/clang++"
cp -aL "$llvm/bin/lld" "$app/usr/toolchain/bin/lld"
ln -sf lld "$app/usr/toolchain/bin/ld.lld"
mkdir -p "$app/usr/toolchain/lib"
cp -a "$llvm/lib/clang" "$app/usr/toolchain/lib/clang"
ldd "$llvm/bin/clang-20" "$llvm/bin/lld" | awk '/=> \//{print $3}' | sort -u | grep -E "LLVM|clang|libedit|libffi|libxml|libz|libzstd|libtinfo|libicu|libbsd|libmd" \
  | while read -r so; do cp -aL "$so" "$app/usr/toolchain/lib/"; done || true

echo "== sysroot (glibc 2.35 + libstdc++ 13, extracted from the .deb files)"
if [ -d "$out/sysroot-cache/usr" ]; then
  echo "(using the sysroot kept in $out/sysroot-cache; delete it to download the packages again)"
  cp -a "$out/sysroot-cache/." "$app/usr/sysroot/"
else
tmp="$(mktemp -d)"; cd "$tmp"
apt-get update >/dev/null  # the builder image ships without package lists
apt-get download libc6 libc6-dev linux-libc-dev libgcc-13-dev libstdc++-13-dev gcc-13-base libgcc-s1 libstdc++6 >/dev/null
for deb in *.deb; do dpkg-deb -x "$deb" "$app/usr/sysroot"; done
# the debs hold absolute symlinks (/lib/x86_64-linux-gnu/...): make them relative so they resolve inside the sysroot
find "$app/usr/sysroot" -type l | while read -r l; do
  t="$(readlink "$l")"
  case "$t" in /*) ln -sfn "$(realpath -m --relative-to="$(dirname "$l")" "$app/usr/sysroot$t")" "$l" ;; esac
done
# merged-/usr layout expected by the toolchain
[ -e "$app/usr/sysroot/lib" ] || ln -s usr/lib "$app/usr/sysroot/lib"
mkdir -p "$out/sysroot-cache" && cp -a "$app/usr/sysroot/." "$out/sysroot-cache/"
cd /; rm -rf "$tmp"
fi
# the runtime libstdc++ / libgcc_s that the built game needs on older distros
cp -aL "$app/usr/sysroot/usr/lib/x86_64-linux-gnu/libstdc++.so.6" "$app/usr/lib/" 2>/dev/null || \
  cp -aL "$app/usr/sysroot/lib/x86_64-linux-gnu/libstdc++.so.6" "$app/usr/lib/"
cp -aL "$app/usr/sysroot/lib/x86_64-linux-gnu/libgcc_s.so.1" "$app/usr/lib/"

echo "== fallback system libraries (used by AppRun only when the host lacks them)"
mkdir -p "$app/usr/fallback"
for so in libX11.so.6 libX11-xcb.so.1 libxcb.so.1 libwayland-client.so.0 libvulkan.so.1; do
  path="$(ldconfig -p | awk -v n="$so" '$1==n && /x86-64/{print $NF}' | sed -n 1p)"
  [ -n "$path" ] || { echo "missing in builder: $so"; exit 1; }
  # the library plus whatever it needs besides glibc / the C++ runtime
  { echo "$path"; ldd "$path" | awk '/=> \//{print $3}'; } | sort -u \
    | grep -v -E '/(libc|libm|libdl|libpthread|librt|libgcc_s|libstdc\+\+)\.so' \
    | while read -r dep; do cp -aL "$dep" "$app/usr/fallback/$(basename "$dep")"; done
done
ls "$app/usr/fallback"

echo "== CMake + Ninja"
cd "$out"
curl -sL -o cmake.tgz https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz
tar -xzf cmake.tgz -C "$app/usr" && mv "$app/usr/cmake-3.31.6-linux-x86_64" "$app/usr/cmake"
rm -rf "$app/usr/cmake/doc" "$app/usr/cmake/man"
curl -sL -o ninja.zip https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-linux.zip
unzip -qo ninja.zip -d "$app/usr/toolchain/bin" && chmod +x "$app/usr/toolchain/bin/ninja"

echo "== portable Python with Tk"
url="$(curl -s https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest \
  | python3 -c "import sys,json,re; a=[x['browser_download_url'] for x in json.load(sys.stdin)['assets']]; \
print([u for u in a if re.search(r'cpython-3\.12\.[0-9]+(\+|%2B).*x86_64-unknown-linux-gnu-install_only\.tar\.gz$', u)][0])")"
curl -sL -o python.tgz "$url"
tar -xzf python.tgz -C "$app/usr"
rm -f python.tgz

echo "== licenses"
mkdir -p "$app/usr/share/doc/regh2"
cp "$repo/LICENSE" "$repo/THIRD_PARTY_NOTICES.md" "$app/usr/share/doc/regh2/"

echo "== AppRun, desktop file, icon"
cp "$repo/packaging/AppRun" "$app/AppRun" && chmod +x "$app/AppRun"
cp "$repo/packaging/toolchain.cmake.in" "$app/usr/app/toolchain.cmake.in"
cp "$repo/packaging/gh2-recomp.desktop" "$app/gh2-recomp.desktop"
if [ -f "$repo/packaging/icon.png" ]; then cp "$repo/packaging/icon.png" "$app/gh2-recomp.png"  # artwork supplied by the maintainer
else python3 "$repo/packaging/make-icon.py" "$app/gh2-recomp.png"; fi

echo "== appimagetool"
curl -sL -o appimagetool https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
chmod +x appimagetool
ARCH=x86_64 ./appimagetool --appimage-extract-and-run --comp zstd "$app" "$out/reGH2-x86_64.AppImage"
ls -la "$out/reGH2-x86_64.AppImage"
