#!/bin/bash
# Builds the prebuilt game host the AppImage ships (usr/host), inside the builder container (Ubuntu 22.04, glibc 2.35, clang 20).
# usage (container): make-host.sh /repo /out /game
#   /out/sdk/linux-amd64  the patched SDK (scripts/build-sdk.sh)
#   /game                 a Guitar Hero II folder (or .iso / default.xex) of the DEVELOPER, mounted read-only: the full build needs the
#                         generated code once to compile the hooks against it. Nothing of the game is staged: the host is gh2 (runtime,
#                         hooks, renderer; 2-3 MB), its GPU plugin, librexruntime.so and the override list (function addresses).
# Writes /out/host (host.json, overrides.txt, gh2, librexgpu-xenos.so, librexruntime.so). The build tree stays in /out/host-build
# (it holds the developer's generated game code: never ship it).
set -euo pipefail
repo="$1"; out="$2"; game="$3"
export PYTHONPATH="$repo/scripts"
export HOME="${HOME:-/root}"
gh2() { python3 -m gh2install --prefix "$out/host-build" --sdk "$out/sdk" --cxx clang++ "$@"; }
gh2 install "$game" --full --rebuild --no-desktop-icon
gh2 make-host --out "$out/host"
# the game code must not be in the staged files: the host is small and has no generated function bodies
size=$(stat -c %s "$out/host/gh2")
if [ "$size" -gt 20000000 ]; then echo "host gh2 is $size bytes: the game code is linked in (GH2_GAME_MODULE off?)" >&2; exit 1; fi
ls -la "$out/host"
