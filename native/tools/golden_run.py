#!/usr/bin/env python3
"""One traced, scripted game run for the golden traces / isolation checks.

  golden_run.py BUILD_DIR SCENARIO LABEL OUT_DIR [SECS]

BUILD_DIR  directory of a build that links the boundary tracer (contains ./gh2)
SCENARIO   title      GH2_SKIP_INTRO=1: logos/intro skipped, stops at the title screen          (default 22 s)
           autostart  GH2_AUTOSTART=1: title -> menus -> Quick Play song loads (nobody plays)    (default 50 s)
           normal     no skipping: logos and the intro video play                                (default 28 s)
Runs work/analysis/scenario_v10.py with GH2_BOUNDARY_TRACE=OUT_DIR/LABEL.trace on a fresh COPY of the saves (the copy of
~/.local/share/gh2 made once in the scratch dir, GH2_USERDATA_PRISTINE), windowed, and reports whether the save files changed.
"""
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys

SCENARIOS = {
    "title": ({"GH2_SKIP_INTRO": "1", "GH2_AUTOSTART": "0"}, 22),
    "autostart": ({"GH2_AUTOSTART": "1", "GH2_AUTOSTART_SECONDS": "30"}, 60),
    "normal": ({"GH2_AUTOSTART": "0"}, 28),
}
ROOT = pathlib.Path(__file__).resolve().parents[3]   # .../work


def tree_hash(path):
    h = hashlib.sha1()
    for p in sorted(pathlib.Path(path).rglob("*")):
        if p.is_file():
            h.update(str(p.relative_to(path)).encode())
            h.update(hashlib.sha1(p.read_bytes()).digest())
    return h.hexdigest()


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    build_dir, scenario, label, out_dir = sys.argv[1:5]
    env_extra, secs = SCENARIOS[scenario]
    if len(sys.argv) > 5:
        secs = float(sys.argv[5])
    pristine = pathlib.Path(os.environ["GH2_USERDATA_PRISTINE"])
    out = pathlib.Path(out_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    work = out / ("userdata_" + label)
    if work.exists():
        shutil.rmtree(work)
    subprocess.run(["cp", "--reflink=auto", "-a", str(pristine), str(work)], check=True)
    saves = "B13EBABEBABEBABE"
    before = tree_hash(work / saves)
    trace = out / (label + ".trace")
    for suffix in ("", ".meta.json", ".counts.json"):
        pathlib.Path(str(trace) + suffix).unlink(missing_ok=True)
    cmd = [sys.executable, "-I", str(ROOT / "analysis" / "scenario_v10.py"), str(secs), label,
           "GH2_BOUNDARY_TRACE=" + str(trace), "GH2_BUILD_DIR=" + str(pathlib.Path(build_dir).resolve()),
           "GH2_TEST_ARGS=--fullscreen=false --user_data_root=" + str(work)]
    cmd += ["%s=%s" % kv for kv in env_extra.items()]
    res = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, GH2_BUILD_DIR=str(pathlib.Path(build_dir).resolve())))
    sys.stdout.write(res.stdout[-1800:])
    if res.returncode != 0 or not res.stdout.strip():
        sys.stdout.write("harness rc=%s stderr: %s\n" % (res.returncode, res.stderr[-800:]))
    after = tree_hash(work / saves)
    print("saves: %s (%s)" % ("UNCHANGED" if before == after else "CHANGED", saves))
    size = trace.stat().st_size if trace.exists() else 0
    print("trace: %s  %.1f MB  counts=%s" % (trace.name, size / 1e6, pathlib.Path(str(trace) + ".counts.json").exists()))
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
