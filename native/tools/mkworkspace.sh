#!/bin/sh
# mkworkspace.sh NAME - a throw-away project directory work/gh2_v10_NAME next to the dev project, with its own copy of generated/.
# A new CMake build directory always re-runs the codegen custom command (its command line contains the build dir, so ninja finds
# no matching log entry); doing that in a copy keeps the dev tree (work/gh2_v10) untouched. Prints the workspace path.
set -e
name="${1:?usage: mkworkspace.sh NAME}"
work="$(cd "$(dirname "$0")/../../.." && pwd)"
ws="$work/gh2_v10_$name"
mkdir -p "$ws"
cd "$ws"
for l in CMakeLists.txt:../gh2-recomp/app/CMakeLists.txt src:../gh2-recomp/app/src hints:../gh2-recomp/app/hints native:../gh2-recomp/native; do
  ln -sfn "${l#*:}" "${l%%:*}"
done
cp -f ../gh2_v10/gh2_manifest.toml .
cp -f ../gh2_v10/run.sh .   # GH2_BUILD=main ./run.sh [--startup_mode=menu ...] runs the build in out/build/main
[ -d generated ] || cp -a ../gh2_v10/generated generated
echo "$ws"
