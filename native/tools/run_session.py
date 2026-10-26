#!/usr/bin/env python3
"""run_session.py LABEL SECONDS [NAME=VALUE | --flag=value ...]   one timed run of the game on a COPY of the saves, log with timestamps.

  NAME=VALUE   environment variable of the game (GH2_AUTOSTART=1, GH2_RENDER_DUMP=/path/prefix, ...)
  --flag=value command line option of the game (--render_resolution=2160p)

Environment of the script itself:
  GH2_BUILD_DIR       build directory with the `gh2` executable (default work/gh2_v10_iso/out/build/main)
  GH2_USERDATA_PRISTINE  a copy of ~/.local/share/gh2 (made on the first run if missing, in OUT_DIR)
  GH2_RUN_OUT         where the logs go (default /tmp/gh2_sessions); LABEL.log
The game gets --fullscreen=false, the shader cache work/analysis/shaders/run5/cache (GH2_SHADER_CACHE overrides) and the native GPU.
The saves of the user are never touched: each run works on its own copy, deleted at the end. Use `GH2_INPUT_*` / the virtual guitar of
gh2_input_selftest (`play`, `script`) for input. Never SIGKILLs a run that ends by itself; at SECONDS it sends SIGTERM, then SIGKILL 10 s later.
"""
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]          # .../work
REPO = ROOT.parent
label, seconds = sys.argv[1], float(sys.argv[2])
extra = sys.argv[3:]
build = pathlib.Path(os.environ.get("GH2_BUILD_DIR", ROOT / "gh2_v10_iso/out/build/main"))
out = pathlib.Path(os.environ.get("GH2_RUN_OUT", "/tmp/gh2_sessions"))
out.mkdir(parents=True, exist_ok=True)
pristine = pathlib.Path(os.environ.get("GH2_USERDATA_PRISTINE", out / "userdata_pristine"))
if not pristine.exists():
    subprocess.run(["cp", "--reflink=auto", "-a", os.path.expanduser("~/.local/share/gh2"), str(pristine)], check=True)
work = out / ("userdata_" + label)
shutil.rmtree(work, ignore_errors=True)
subprocess.run(["cp", "--reflink=auto", "-a", str(pristine), str(work)], check=True)

env = dict(os.environ, GH2_TIMELINE="1")
env["LD_LIBRARY_PATH"] = str(ROOT / "sdk-built/linux-amd64/lib") + ":" + str(build)
env.setdefault("GH2_SHADER_CACHE", str(ROOT / "analysis/shaders/run5/cache"))
args = []
for item in extra:
    if item.startswith("--"):
        args.append(item)
    else:
        k, v = item.split("=", 1)
        env[k] = v
cmd = [str(build / "gh2"), "--game_data_root=" + str(REPO / "game/extracted"), "--gpu_plugin=xenos", "--fullscreen=false", "--user_data_root=" + str(work), "--gpu=native"] + args
proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, text=True, errors="replace", start_new_session=True, cwd=build)
t0 = time.monotonic()
log = open(out / (label + ".log"), "w")


def reader():
    for line in proc.stdout:
        log.write("%7.2f %s\n" % (time.monotonic() - t0, line.rstrip()))
        log.flush()


thread = threading.Thread(target=reader, daemon=True)
thread.start()
while time.monotonic() - t0 < seconds and proc.poll() is None:
    time.sleep(0.5)
if proc.poll() is None:
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except OSError:
        pass
    try:
        proc.wait(10)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
proc.wait()
thread.join(3)
log.close()
shutil.rmtree(work, ignore_errors=True)
print("%s: exit %s after %.0f s, log %s" % (label, proc.returncode, time.monotonic() - t0, out / (label + ".log")))
