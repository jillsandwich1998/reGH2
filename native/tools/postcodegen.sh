#!/bin/sh
# postcodegen.sh GENERATED_DIR - run after every `rexglue codegen` (same edit as Pipeline.codegen() in scripts/gh2install/pipeline.py).
# The recompiler turns the PowerPC SMT yield hint `db16cyc` into a bare comment, so the game's spin-waits and thread barriers burn a
# core at full speed. The comment becomes a call to gh2_spin_hint(), defined by native/rt/hooks (PAUSE + sleep after a long spin).
dir="${1:?usage: postcodegen.sh GENERATED_DIR  (e.g. work/gh2_v10/generated/default)}"
n=0
for f in "$dir"/gh2_recomp.*.cpp; do
  if grep -q '^[[:space:]]*// db16cyc' "$f"; then
    sed -i 's|^\([[:space:]]*\)// db16cyc.*$|\1{ void gh2_spin_hint(); gh2_spin_hint(); }  // db16cyc|' "$f"
    n=$((n+1))
  fi
done
echo "postcodegen: patched $n files in $dir"
