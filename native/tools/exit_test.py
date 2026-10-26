#!/usr/bin/env python3
"""exit_test.py SIGNAL [SECONDS] [--pipe]   does the game really end (and give its memory back) when it is asked to close?

Starts the game on a COPY of the saves (autostart Quick Play, 1080p), lets it run SECONDS (default 25), sends SIGNAL (INT = Ctrl+C in a terminal, TERM, HUP) and measures:
  - how long until the process is gone (and whether anything of it is still alive 5 s later: processes whose executable is the game's, children, zombies),
  - the system's MemAvailable and the resident size of the game before and after, and what is left in /dev/shm and the game's temp files.
--pipe: the game runs as `run.sh ... 2>&1 | tee file` like the user starts it, and the signal goes to the whole process group (what Ctrl+C does).
Environment: GH2_BUILD_DIR as in run_session.py. Prints a summary only; the saves of the user are never touched. Never uses SIGKILL on the game: if it is still there after 40 s it
reports that and leaves it for the caller to decide."""
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]
REPO = ROOT.parent
sig = getattr(signal, "SIG" + sys.argv[1].upper())
args = [a for a in sys.argv[2:] if not a.startswith("--")]
seconds = float(args[0]) if args else 25.0
via_pipe = "--pipe" in sys.argv
build = pathlib.Path(os.environ.get("GH2_BUILD_DIR", ROOT / "gh2_v10_iso/out/build/main"))
out = pathlib.Path(os.environ.get("GH2_RUN_OUT", "/tmp/gh2_sessions"))
out.mkdir(parents=True, exist_ok=True)
pristine = pathlib.Path(os.environ.get("GH2_USERDATA_PRISTINE", out / "userdata_pristine"))
if not pristine.exists():
    subprocess.run(["cp", "--reflink=auto", "-a", os.path.expanduser("~/.local/share/gh2"), str(pristine)], check=True)
work = out / "userdata_exit"
shutil.rmtree(work, ignore_errors=True)
subprocess.run(["cp", "--reflink=auto", "-a", str(pristine), str(work)], check=True)

exe = str(build / "gh2")
env = dict(os.environ, GH2_AUTOSTART="1", GH2_TIMELINE="1")
env["LD_LIBRARY_PATH"] = str(ROOT / "sdk-built/linux-amd64/lib") + ":" + str(build)
env.setdefault("GH2_SHADER_CACHE", str(ROOT / "analysis/shaders/run5/cache"))
cmd = [exe, "--game_data_root=" + str(REPO / "game/extracted"), "--gpu_plugin=xenos", "--fullscreen=false", "--user_data_root=" + str(work), "--gpu=native", "--render_resolution=1080p"]
log = str(out / "exit_test.log")


def meminfo(key):
    for line in open("/proc/meminfo"):
        if line.startswith(key + ":"):
            return int(line.split()[1]) // 1024
    return -1


def game_pids():
    pids = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            if os.readlink("/proc/%s/exe" % entry).startswith(str(build)) or os.path.realpath("/proc/%s/exe" % entry) == os.path.realpath(exe):
                pids.append(int(entry))
        except OSError:
            pass
    return pids


def rss_mb(pid):
    try:
        for line in open("/proc/%d/status" % pid):
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) // 1024
    except OSError:
        pass
    return 0


def shm():
    return sorted(os.listdir("/dev/shm"))


def parts():
    return "MemAvailable %d, AnonPages %d, Shmem %d, Cached %d MB" % tuple(meminfo(k) for k in ("MemAvailable", "AnonPages", "Shmem", "Cached"))


before_free, shm_before = meminfo("MemAvailable"), shm()
print("before the start:", parts())
if via_pipe:
    shell = "exec %s 2>&1 | tee %s" % (" ".join(cmd), log)
    proc = subprocess.Popen(["bash", "-c", shell], env=env, start_new_session=True, stdout=subprocess.DEVNULL, cwd=build)
else:
    proc = subprocess.Popen(cmd, env=env, start_new_session=True, stdout=open(log, "w"), stderr=subprocess.STDOUT, cwd=build)
t0 = time.monotonic()
time.sleep(seconds)
pids = game_pids()
running_free, running_rss = meminfo("MemAvailable"), sum(rss_mb(p) for p in pids)
print("running:", parts())
print("running: %d game process(es) %s, resident %d MB, MemAvailable %d MB (was %d before the start)" % (len(pids), pids, running_rss, running_free, before_free))
t_sig = time.monotonic()
os.killpg(proc.pid, sig) if via_pipe else os.kill(proc.pid, sig)
gone = None
while time.monotonic() - t_sig < 40:
    if not game_pids() and proc.poll() is not None:
        gone = time.monotonic() - t_sig
        break
    time.sleep(0.1)
if gone is None:
    print("STILL ALIVE 40 s after SIG%s: pids %s" % (sys.argv[1].upper(), game_pids()))
else:
    print("SIG%s: every game process was gone after %.2f s (exit status %s)" % (sys.argv[1].upper(), gone, proc.returncode))
time.sleep(5)
left = game_pids()
zombies = [e for e in os.listdir("/proc") if e.isdigit() and os.path.exists("/proc/%s/stat" % e) and open("/proc/%s/stat" % e).read().split(")")[-1].split()[0] == "Z" and "gh2" in open("/proc/%s/stat" % e).read().split(")")[0]]
after_free = meminfo("MemAvailable")
print("5 s later:", parts())
print("5 s later: game processes left %s, zombies %s, MemAvailable %d MB (running %d, before start %d), /dev/shm new entries %s" % (left, zombies, after_free, running_free, before_free, [s for s in shm() if s not in shm_before]))
shutil.rmtree(work, ignore_errors=True)
