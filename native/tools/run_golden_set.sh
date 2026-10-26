#!/bin/sh
# run_golden_set.sh BUILD_DIR OUT_DIR PREFIX [REPS]
# 3 scenarios x REPS (default 3) traced runs: <OUT_DIR>/<PREFIX>_<scenario>_<n>.trace (+ .meta.json / .counts.json) and one result
# line per run in <OUT_DIR>/<PREFIX>_summary.txt. Needs GH2_USERDATA_PRISTINE (a copy of ~/.local/share/gh2; never the real one).
set -e
build="${1:?BUILD_DIR}"; out="${2:?OUT_DIR}"; prefix="${3:?PREFIX}"; reps="${4:-3}"
: "${GH2_USERDATA_PRISTINE:?set GH2_USERDATA_PRISTINE to a copy of ~/.local/share/gh2}"
tools="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$out"
: > "$out/${prefix}_summary.txt"
for scenario in title autostart normal; do
  n=1
  while [ "$n" -le "$reps" ]; do
    label="${prefix}_${scenario}_${n}"
    echo "== $label" | tee -a "$out/${prefix}_summary.txt"
    python3 "$tools/golden_run.py" "$build" "$scenario" "$label" "$out" 2>&1 | grep -E '^(saves|trace):|exit=|harness' | tee -a "$out/${prefix}_summary.txt"
    n=$((n+1))
    sleep 2
  done
done
echo "done" | tee -a "$out/${prefix}_summary.txt"
