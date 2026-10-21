<p align="center"><img src="packaging/icon.png" width="160" alt="reGH2 logo"></p>

<h1 align="center">reGH2</h1>

Static recompilation of Guitar Hero 2 (Xbox 360) for PC. Linux with Vulkan for now.

> Unofficial fan project, not affiliated with or endorsed by Activision or Harmonix. It contains nothing of the game: you
> bring your own copy of *Guitar Hero II* (Xbox 360, USA) and it is rebuilt on your computer.

A native port of **Guitar Hero II (Xbox 360)** built with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).
You need **your own copy of the game**: this project ships no game files and no code generated from them. The installer
recompiles the game on your computer from the disc image (.iso), the extracted game folder, or `default.xex`.

## Using it (AppImage)

1. Download `reGH2-x86_64.AppImage`, `chmod +x` it, run it.
2. Press **Select folder...** and choose the folder that holds your game (the disc image, the extracted game, or the folder with `default.xex`; it is searched for inside), then **Install** (a few minutes: the game is compiled here).
3. Press **PLAY**. In the game press **F2** for the options (graphics, display mode, controllers, keyboard, DLC, start-up, FPS counter) and to **Quit game** (it asks for confirmation).

Command line: `reGH2-x86_64.AppImage install <iso|folder|default.xex>`, `... launch [game options]`, `... status`, `... doctor`, `... report`, `... saves`, `... uninstall`.

## Testing and reporting problems

This is an early pre-release and it has been played on a single computer. **It is expected that you play it and test it on your
own machine**, and that anything that does not work is reported as a [GitHub issue](https://github.com/jillsandwich1998/reGH2/issues/new/choose). A good report lets us
reproduce and analyse the problem:

1. **Reproduce the problem** (or keep the game open right after it happened).
2. **Say at what moment of the game it happens**: the menu or the song (and its difficulty), what was on screen, and anything unusual
   just before, such as an achievement appearing or a change in the options.
3. **Attach the problem report**:
   * In the game press **F2**, open the **GAME** tab and press **Save problem report**, right after the problem.
   * If the game does not start or has crashed, use **Save problem report** in the launcher, or run
     `reGH2-x86_64.AppImage report`.

   The report is one text file with your system, how the game was compiled, the settings in use, the controllers, the game's frame
   rate and the end of its logs. In the game it can also save a screenshot (a PNG next to the report, with a tick box to turn it
   off): only the game's own picture, never your desktop or other windows. Attach the PNG too. Your home folder is replaced by `~`; open the file and check it if you want before sharing it.
4. Include your graphics card, its driver and your Linux distribution if the report cannot tell them (it usually can).

## Saves and uninstalling

* Your save game, achievements and installed DLC live in `~/.local/share/gh2`. In the game, **F2 > GAME > Open saves folder** opens it; in
  the launcher, **Open saves folder** does the same and **Export saves...** copies the save game and achievements into a `.zip`
  (`reGH2-x86_64.AppImage saves --export <folder>` from the command line). The DLC, which can be installed again, is not in the zip.
* **Uninstall...** in the launcher (or `reGH2-x86_64.AppImage uninstall`) removes the installed game, its shortcuts and its cache. It keeps
  your saves, DLC and achievements unless you say otherwise, and it never touches your own copy of the game. Close the game first.

## Requirements

Linux x86-64 (glibc 2.35 or newer; SteamOS / Steam Deck included), a Vulkan 1.2 GPU with a current Mesa or NVIDIA driver,
about 4 GB of free disk space for the build and your game files, and a few GB of RAM while compiling.

Nothing else needs installing by hand. The AppImage carries the compiler, the C++ runtime, the X11 / Wayland client
libraries and the Vulkan loader (host copies are preferred when present). The one thing it cannot carry is the Vulkan
*driver* for your GPU, which is part of your system. If none is found, the launcher (and `doctor` / `launch` on the
command line) says so, shows the package for your distribution and GPU, and installs it only if you agree, asking for
your administrator password:

| Distribution family | AMD / Intel | NVIDIA |
|---|---|---|
| Debian, Ubuntu, Mint, ... | `mesa-vulkan-drivers` | the NVIDIA driver from your distribution |
| Fedora, RHEL, ... | `mesa-vulkan-drivers` | the NVIDIA driver (e.g. from RPM Fusion) |
| Arch, CachyOS, Manjaro, ... | `vulkan-radeon` / `vulkan-intel` | `nvidia-utils` |
| openSUSE | `libvulkan_radeon` / `libvulkan_intel` | the NVIDIA driver |

Frame pacing is smoothest on **Linux 6.11 or newer**: the SDK's memory-protection queries are answered by the kernel directly there. Older kernels still work, with the previous, slower path (in our measurements it made a share of frames arrive late in pairs).

The game also needs a working audio server (PipeWire, PulseAudio or ALSA) and a display (Wayland or X11), as any desktop has.

## Building from source

```sh
scripts/build-sdk.sh ~/gh2-sdk                         # patched ReXGlue SDK (pinned commit + sdk-patches/)
scripts/gh2-recomp --sdk ~/gh2-sdk install <your game>   # codegen + build, needs clang >= 20, cmake, ninja
scripts/gh2-recomp --sdk ~/gh2-sdk launch
```
The AppImage is built by `packaging/` (Ubuntu 22.04 container, so it runs on older distributions as well).

## Supported game version

`default.xex` with SHA-1 `b90b864487e6291761b207c8d0064e9a0779bdee` (Xbox 360, title 415607E7). Other versions are refused
unless you pass `--force`.

## Notes

DLC (Xbox Live LIVE/CON packages) can be installed from the in-game menu, one by one or from a whole folder.

License: MIT (see `LICENSE`); third-party notices: `THIRD_PARTY_NOTICES.md`.

## Icon

`packaging/icon.svg` (and the rendered `icon.png`) is original artwork made for this project and covered by the same MIT licence.
This is an unofficial project, not affiliated with or endorsed by Activision or Harmonix.

## Tools for finding performance problems

* **FPS counter:** F2 > GAME > *Show FPS counter*.
* **Slowdown log:** whenever the game itself runs below about 55 fps for a second, or one frame takes over 50 ms, a line with the
  time goes to `logs/perf.log` next to the game. The problem report includes it.
* **Audio:** the audio buffers are the smallest that worked well on the test machine (about 27 ms in total). If you hear crackling,
  start the game with `--audio_latency=balanced` (or `safe`) and tell us about your hardware.

