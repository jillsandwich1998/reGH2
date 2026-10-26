#!/usr/bin/env python3
"""What the game really plays: records the game's own PulseAudio/PipeWire stream (parec --monitor-stream) and measures it.

  audio_capture.py BUILD_DIR OUT_DIR OUTPUT [START_S] [SECS] [SCENARIO]
  audio_capture.py --compare OUT_DIR            compares OUT_DIR/sdk.json with OUT_DIR/native.json

OUTPUT    sdk | native      (the game's --audio_output)
START_S   seconds after launch at which the recording starts (default 9); SECS its length (default 16)
SCENARIO  normal (default: logos + intro video, deterministic) or song (GH2_AUTOSTART, a Quick Play song; use START_S around 55)
The game runs on a COPY of the saves (GH2_USERDATA_PRISTINE) in a window. The recording is the stream as the sound server receives it
(6 float32 channels, FL FR FC LFE BL BR), before the device. Per channel it reports RMS and peak in dBFS and the RMS of every second;
--compare prints the level difference per channel and the correlation of the per-second envelopes of the two runs.
Standard library only (no numpy on this machine).
"""
import array
import json
import math
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]          # .../work
REPO = ROOT.parent
CHANNELS = ["FL", "FR", "FC", "LFE", "BL", "BR"]


def db(x):
    return 20 * math.log10(x) if x > 1e-9 else -180.0


def find_sink_input(pid_hint=None, timeout=60.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        out = subprocess.run(["pactl", "list", "sink-inputs"], capture_output=True, text=True).stdout
        for block in re.split(r"\n(?=Sink Input #)", out):
            m = re.match(r"Sink Input #(\d+)", block)
            if not m:
                continue
            if 'application.name = "rexglue"' in block or (pid_hint and 'application.process.id = "%d"' % pid_hint in block):
                return int(m.group(1)), block
        time.sleep(0.5)
    return None, ""


def analyse(raw_path):
    data = array.array("f")
    with open(raw_path, "rb") as f:
        data.frombytes(f.read())
    n = len(data) // 6
    res = {"samples_per_channel": n, "seconds": n / 48000.0, "channels": {}}
    seconds = int(n // 48000)
    for c, name in enumerate(CHANNELS):
        ch = data[c::6]
        peak = max((abs(v) for v in ch), default=0.0)
        total = math.fsum(v * v for v in ch)
        per_sec = []
        for s in range(seconds):
            seg = ch[s * 48000:(s + 1) * 48000]
            per_sec.append(round(db(math.sqrt(math.fsum(v * v for v in seg) / len(seg))), 2))
        res["channels"][name] = {"rms_db": round(db(math.sqrt(total / max(n, 1))), 2), "peak_db": round(db(peak), 2), "rms_db_per_second": per_sec}
    return res


def pearson(a, b):
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    ma, mb = sum(a) / n, sum(b) / n
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    den = math.sqrt(sum((x - ma) ** 2 for x in a) * sum((y - mb) ** 2 for y in b))
    return num / den if den else float("nan")


def compare(out):
    a, b = json.load(open(out / "sdk.json")), json.load(open(out / "native.json"))
    print("channel   sdk rms/peak dBFS      native rms/peak dBFS    delta rms   envelope correlation")
    for name in CHANNELS:
        x, y = a["channels"][name], b["channels"][name]
        corr = pearson(x["rms_db_per_second"], y["rms_db_per_second"])
        print("%-5s    %7.2f / %7.2f       %7.2f / %7.2f       %+6.2f dB   %.4f" % (name, x["rms_db"], x["peak_db"], y["rms_db"], y["peak_db"], y["rms_db"] - x["rms_db"], corr))
    print("seconds recorded: sdk %.1f, native %.1f" % (a["seconds"], b["seconds"]))


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--compare":
        return compare(pathlib.Path(sys.argv[2]))
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    build, out, output = pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve(), sys.argv[3]
    start_s = float(sys.argv[4]) if len(sys.argv) > 4 else 9.0
    secs = float(sys.argv[5]) if len(sys.argv) > 5 else 16.0
    scenario = sys.argv[6] if len(sys.argv) > 6 else "normal"
    if output not in ("sdk", "native"):
        sys.exit("OUTPUT is sdk or native")
    pristine = pathlib.Path(os.environ["GH2_USERDATA_PRISTINE"])
    out.mkdir(parents=True, exist_ok=True)
    work = out / ("userdata_" + output)
    if work.exists():
        shutil.rmtree(work)
    subprocess.run(["cp", "--reflink=auto", "-a", str(pristine), str(work)], check=True)
    env = dict(os.environ, LD_LIBRARY_PATH="%s:%s" % (ROOT / "sdk-built/linux-amd64/lib", build), GH2_AUDIO_TRACE="1")
    if scenario == "song":
        env.update({"GH2_AUTOSTART": "1", "GH2_AUTOSTART_SECONDS": "30"})
    log = open(out / (output + "_game.out"), "w")
    t_launch = time.time()
    game = subprocess.Popen([str(build / "gh2"), "--game_data_root=" + str(REPO / "game/extracted"), "--gpu_plugin=xenos", "--fullscreen=false",
                             "--user_data_root=" + str(work), "--audio_output=" + output], env=env, stdout=log, stderr=subprocess.STDOUT, cwd=build,
                            start_new_session=True)
    try:
        idx, block = find_sink_input(game.pid)
        if idx is None:
            sys.exit("the game never opened an audio stream")
        (out / (output + "_stream.txt")).write_text(block)
        while time.time() - t_launch < start_s:
            time.sleep(0.05)
        raw = out / (output + ".raw")
        rec = subprocess.Popen(["parec", "--monitor-stream=%d" % idx, "--format=float32le", "--rate=48000", "--channels=6",
                                "--channel-map=front-left,front-right,front-center,lfe,rear-left,rear-right", "--raw"], stdout=open(raw, "wb"),
                               stderr=subprocess.DEVNULL)
        time.sleep(secs)
        rec.send_signal(signal.SIGINT)
        rec.wait(timeout=10)
    finally:
        os.killpg(game.pid, signal.SIGTERM)
        try:
            game.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(game.pid, signal.SIGKILL)
    shutil.rmtree(work, ignore_errors=True)   # the copy of the saves is 270 MB and the scratch directory is often a tmpfs
    res = analyse(out / (output + ".raw"))
    res.update({"output": output, "scenario": scenario, "start_s": start_s, "stream": idx})
    (out / (output + ".json")).write_text(json.dumps(res, indent=1) + "\n")
    print("%-6s recorded %.1f s from stream #%d | rms dBFS per channel: %s | peak %s" %
          (output, res["seconds"], idx, " ".join("%s %.1f" % (n, res["channels"][n]["rms_db"]) for n in CHANNELS),
           " ".join("%.1f" % res["channels"][n]["peak_db"] for n in CHANNELS)))


if __name__ == "__main__":
    main()
