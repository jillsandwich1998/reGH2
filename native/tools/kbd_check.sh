#!/bin/sh
# kbd_check.sh BUILD_DIR OUT_LOG - the keyboard path end to end, with a virtual keyboard (uinput) typing into the game.
# Starts BUILD_DIR/gh2 FULLSCREEN (so it has the keyboard focus) with GH2_INPUT_TRACE=1 on a COPY of the saves, types
# down, up, A, S, D, F, G, Enter, Space at fixed seconds (work/analysis/kbd_inject.py) and leaves the game's stderr in OUT_LOG.
# With no guitar or gamepad plugged in the keyboard plays as player 1 (keyboard_player = auto). Compare two builds with:
#   grep -E '^(PAD|CAPS)' LOG | sed 's/ at [0-9.]*s//'
# Needs GH2_USERDATA_PRISTINE (a copy of ~/.local/share/gh2) and access to /dev/uinput.
set -e
build="$(cd "${1:?BUILD_DIR}" && pwd)"; log="$(realpath -m "${2:?OUT_LOG}")"
: "${GH2_USERDATA_PRISTINE:?set GH2_USERDATA_PRISTINE to a copy of ~/.local/share/gh2}"
work="$(cd "$(dirname "$0")/../../.." && pwd)"
root="$(dirname "$work")"
copy="$(mktemp -d "${TMPDIR:-/tmp}/gh2kbd.XXXXXX")"
trap 'rm -rf "$copy"' EXIT
cp --reflink=auto -a "$GH2_USERDATA_PRISTINE" "$copy/userdata"
cd "$build"
LD_LIBRARY_PATH="$work/sdk-built/linux-amd64/lib:$build" GH2_INPUT_TRACE=1 KBD_LOG="$log" \
STEPS="12:down,13.5:up,15:a,16.5:s,18:d,19.5:f,21:g,22.5:enter,24:space" \
python3 "$work/analysis/kbd_inject.py" -- ./gh2 --game_data_root="$root/game/extracted" --gpu_plugin=xenos \
  --user_data_root="$copy/userdata" --startup_mode=menu
echo "kbd_check: $(grep -c '^PAD' "$log") PAD lines, $(grep -c '^CAPS' "$log") CAPS lines in $log"
