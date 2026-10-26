#!/usr/bin/env python3
"""Press-to-guest latency of a controller reader, measured in the real game.

  input_ab.py BUILD_DIR OUT_DIR READER [SECS] [HZ] [SCENARIO]

BUILD_DIR  a build directory with ./gh2 (the boundary tracer is not needed)
READER     sdk | native      (the game's --input_reader)
SECS       how long the injector presses (default 40); HZ: presses per second, about (default 6)
SCENARIO   title (default: the game idles at the title screen, 60 polls a second) or song (GH2_AUTOSTART: it walks into a Quick Play
           song, nobody plays; the injector starts when the song is running, the UI and GPU threads are at their busiest)

A uinput guitar ("GH2 Injector Guitar", ids 1430:4749) toggles the orange fret at random phases and logs the steady-clock
nanoseconds of every write(); the game's XamInputGetState hook (GH2_INPUT_LOG) logs the nanoseconds of the first poll that returned
each change. The real X-plorer, if plugged in, is hidden from the game (SDL_GAMECONTROLLER_IGNORE_DEVICES) so the injector is player 1.
Needs GH2_USERDATA_PRISTINE (a copy of ~/.local/share/gh2; the game runs on a COPY of it, never on the real saves) and write
access to /dev/uinput. Prints the latency distribution (injected -> seen by the guest) and writes OUT_DIR/READER.json.
"""
import json
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]          # .../work
REPO = ROOT.parent
SELFTEST = ROOT / "gh2-recomp/native/third_party/build/input_sdl3/gh2_input_selftest"
ORANGE = 0x0100


def percentile(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p / 100.0 * len(v)))] if v else float("nan")


def parse(path, cols):
    rows = []
    for line in open(path):
        if line.startswith("#") or not line.strip():
            continue
        rows.append([int(x, 16) if i == cols - 1 and cols == 4 else int(x) for i, x in enumerate(line.split()[:cols])])
    return rows


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    build = pathlib.Path(sys.argv[1]).resolve()
    out = pathlib.Path(sys.argv[2]).resolve()
    reader = sys.argv[3]
    secs = float(sys.argv[4]) if len(sys.argv) > 4 else 40.0
    hz = int(sys.argv[5]) if len(sys.argv) > 5 else 6
    scenario = sys.argv[6] if len(sys.argv) > 6 else "title"
    if scenario not in ("title", "song"):
        sys.exit("SCENARIO is title or song")
    if reader not in ("sdk", "native"):
        sys.exit("READER is sdk or native")
    pristine = pathlib.Path(os.environ["GH2_USERDATA_PRISTINE"])
    out.mkdir(parents=True, exist_ok=True)
    work = out / ("userdata_" + reader)
    if work.exists():
        shutil.rmtree(work)
    subprocess.run(["cp", "--reflink=auto", "-a", str(pristine), str(work)], check=True)
    guest_log, inj_log, game_out = out / (reader + "_guest.log"), out / (reader + "_injector.log"), out / (reader + "_game.out")
    for p in (guest_log, inj_log):
        p.unlink(missing_ok=True)

    env = dict(os.environ)
    env.update({
        "LD_LIBRARY_PATH": "%s:%s" % (ROOT / "sdk-built/linux-amd64/lib", build),
        "GH2_INPUT_LOG": str(guest_log), "GH2_TIMELINE": "1", "GH2_FRAME_TRACE": "1",
        "SDL_GAMECONTROLLER_IGNORE_DEVICES": "0x1430/0x4748",
    })
    if scenario == "title":
        env.update({"GH2_SKIP_INTRO": "1", "GH2_AUTOSTART": "0"})
    else:
        env.update({"GH2_AUTOSTART": "1", "GH2_AUTOSTART_SECONDS": "30"})
    # The injector exists before the game starts (so it is a device from the first poll) and starts pressing `delay` seconds later.
    delay = float(os.environ.get("GH2_INAB_DELAY", 24.0 if scenario == "title" else 52.0))   # seconds after launch; the song now starts at ~16-20 s, so 24 s keeps a whole run near 45 s
    inj = subprocess.Popen([str(SELFTEST), "inject", str(hz), str(secs), str(inj_log), str(delay)], stdout=subprocess.DEVNULL,
                           stderr=open(out / (reader + "_injector.err"), "w"))
    time.sleep(1.5)
    game = subprocess.Popen([str(build / "gh2"), "--game_data_root=" + str(REPO / "game/extracted"), "--gpu_plugin=xenos", "--fullscreen=false",
                             "--user_data_root=" + str(work), "--input_reader=" + reader], env=env, stdout=open(game_out, "w"),
                            stderr=subprocess.STDOUT, cwd=build, start_new_session=True)
    try:
        inj.wait(timeout=delay + secs + 20)
    except subprocess.TimeoutExpired:
        inj.kill()
    time.sleep(1.0)
    os.killpg(game.pid, signal.SIGTERM)
    try:
        game.wait(timeout=15)
        game_rc = game.returncode
    except subprocess.TimeoutExpired:
        os.killpg(game.pid, signal.SIGKILL)
        game_rc = "killed"

    shutil.rmtree(work, ignore_errors=True)   # the copy of the saves is 270 MB and the scratch directory is often a tmpfs
    injected = parse(inj_log, 2) if inj_log.exists() else []
    seen = [r for r in (parse(guest_log, 4) if guest_log.exists() else []) if r[1] == 0]   # user 0 = the injector
    lat_ms, missed = [], 0
    j = 0
    for t_inj, state in injected:
        want = ORANGE if state else 0   # the orange bit only: the autostart scenario presses A in the same word
        while j < len(seen) and (seen[j][0] < t_inj or (seen[j][3] & ORANGE) != want):
            j += 1
        if j >= len(seen):
            missed += 1
            continue
        lat_ms.append((seen[j][0] - t_inj) / 1e6)
        j += 1
    res = {"reader": reader, "scenario": scenario, "injected": len(injected), "matched": len(lat_ms), "missed": missed, "game_exit": game_rc,
           "p50_ms": percentile(lat_ms, 50), "p90_ms": percentile(lat_ms, 90), "p99_ms": percentile(lat_ms, 99),
           "max_ms": max(lat_ms) if lat_ms else float("nan"), "mean_ms": sum(lat_ms) / len(lat_ms) if lat_ms else float("nan"),
           "min_ms": min(lat_ms) if lat_ms else float("nan")}
    (out / (reader + "_" + scenario + ".json")).write_text(json.dumps(res, indent=1) + "\n")
    print("%-6s %-5s injected %3d, seen by the guest %3d (missed %d) | latency ms: min %5.1f  p50 %5.1f  mean %5.1f  p90 %5.1f  p99 %5.1f  max %5.1f | game exit %s"
          % (reader, scenario, res["injected"], res["matched"], missed, res["min_ms"], res["p50_ms"], res["mean_ms"], res["p90_ms"], res["p99_ms"], res["max_ms"], game_rc))
    if not lat_ms:
        print("  no change reached the guest: is the game window focused? (a window without focus reads a neutral pad)")
        sys.exit(1)


if __name__ == "__main__":
    main()
