<p align="center"><img src="packaging/icon.png" width="160" alt="reGH2 logo"></p>

<h1 align="center">reGH2</h1>

Guitar Hero II (Xbox 360) on the PC, rebuilt from your own copy of the game. Linux with Vulkan for now.

> Unofficial fan project, not affiliated with or endorsed by Activision or Harmonix. It contains nothing of the game: you
> bring your own copy of *Guitar Hero II* (Xbox 360, USA) and it is rebuilt on your computer. No game file, and no code
> made from the game, is ever distributed.

**Status: pre-release (0.2.0).** It has been played in Quick Play and in Career mode up to the fourth venue, with a real
X-plorer guitar, on one computer (AMD Radeon RX 6600 XT, Mesa RADV, Linux). It has **not** been played from start to finish, so
nothing here is promised to work in every song, venue or game mode. It is also **not** known how it behaves on NVIDIA or Intel
graphics or on SteamOS. See *Testing and reporting problems* and *Known limits*.

## A native Vulkan renderer, no emulation layer

Earlier versions (0.1.x) ran the recompiled game on the runtime of the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), which
is derived from the Xenia emulator. Kernel services, memory, threads, graphics, sound and controllers all went through emulation
layers, and the graphics could only be scaled by whole multiples of the console's 720p. **From 0.2.0 the game runs on this
project's own native code instead**, and draws with its own **Vulkan renderer** at true 1080p, 1440p and 4K.

| Layer | 0.1.x (ReXGlue runtime) | 0.2.0 (this project) |
|---|---|---|
| Game CPU code | recompiled | recompiled (same) |
| Kernel services, memory, threads, files, synchronisation | emulated by the SDK runtime | `native/kernel`, `native/rt`; the game's threads, including the main one, are our own |
| Graphics | the SDK's GPU emulation, integer resolution scales only | a native Vulkan renderer (`native/gfx`) that turns the console's GPU command stream into Vulkan calls; the game's shaders are translated from your copy with [XenosRecomp](https://github.com/hedge-dev/XenosRecomp); native 1080p, 1440p and 4K |
| Audio, controllers | the SDK's | SDL3 (`native/platform`) |

So the game no longer depends on the ReXGlue **runtime**. The SDK is still used for two things that do not take part in running the
game's code or drawing its frames: its **code generator** (it turns the game's PowerPC code into C++ once, on your machine) and the
**application shell** (window, in-game menu, saves, achievements, DLC).

### What that gives

Measured on the test machine (Ryzen 5600, RX 6600 XT), Quick Play, same moment of the same song, one run each:

* **Native 1080p and 4K** (and 1440p, or any height you choose), with MSAA, anisotropic filtering and vsync selectable in the game
  without restarting. In a 19-minute session at 4K (four songs) the average was 59.4 fps, the GPU used 9.7 ms per frame and nothing crashed.
* **60.0 fps, the engine's own limit.** The game's logic is tied to 60 Hz, as on the console, so 60 fps is the most it can run
  at; the options change how the frame is drawn, not the game's speed. The renderer keeps that rate with 16.7 ms between frames
  and no dropped frames.
* **Less load on the graphics card and memory** than the SDK's graphics path at 720p: the GPU was busy about 26% of the time
  against 38%, and the game process used about 530 MB of RAM against 600 MB. (The native renderer uses *more* CPU than the SDK's
  did, about one core against 0.65 of a core, because it does the work itself; the game's own recompiled code takes under a fifth
  of a core.)
* **No emulator start-up costs**: only the one-time shader translation (about 11 seconds) on the first start.

These are one machine's numbers, not a benchmark: please report yours.

## How it is built

The game's PowerPC code is translated to C++ once, on your machine, from your own `default.xex` (static recompilation), and
compiled with a compiler that comes inside the AppImage.

## Using it (AppImage)

1. Download `reGH2-x86_64.AppImage`, `chmod +x` it, run it.
2. Press **Select folder...** and choose the folder that holds your game (the disc image, the extracted game, or the folder with `default.xex`; it is searched for inside), then **Install** (a few minutes: the game is compiled here).
3. Press **PLAY**. The first start also translates the game's shaders for your graphics card (about 15 seconds, once, with a progress window; nothing is downloaded). In the game press **F2** for the options and to **Quit game** (it asks for confirmation).

Nothing else has to be installed or downloaded: the AppImage carries the compiler, the C++ runtime, the shader tools, Python and the
libraries it needs. The one thing it cannot carry is the Vulkan *driver* for your graphics card (see Requirements).

Command line: `reGH2-x86_64.AppImage install <iso|folder|default.xex>`, `... launch [game options]`, `... status`, `... doctor`, `... report`, `... saves`, `... uninstall`, `... update`.

### In the game

* **F2**: options. *GRAPHICS*: internal resolution (720p to 4K), anti-aliasing (MSAA), anisotropic filtering, vertical sync; display mode; controllers, keyboard, DLC, start-up, FPS counter, audio latency.
* **Guitar or gamepad in the menus**: hold **START + BACK** for about 0.7 s to open or close the options; strum up/down moves, green selects, red goes back, yellow/orange change tab (gamepad: bumpers change tab, left stick moves).
* **F9**: saves a screenshot of the game's own picture to `~/gh2_shots`.
* The game is limited to 60 frames per second by its own engine, as on the console; the options change how it is drawn (resolution, vsync), not the speed.

**Updates.** When the launcher opens it looks on GitHub for a newer release (one HTTPS request to the GitHub API, nothing else is sent; a tick box at the bottom of the launcher turns it off, or set `GH2_NO_UPDATE_CHECK=1`). If there is one, a bar offers **Update now**: it downloads the new AppImage, checks it against the release's `SHA256SUMS.txt`, replaces the old one and restarts the launcher. Then **Update game** rebuilds your installed game with the new version from the game files it was built from (a few minutes; saves are kept). Nothing is updated without your click. `reGH2-x86_64.AppImage update` shows whether a newer version exists and `update --apply` downloads it. The checksum comes from the same release, so it detects a damaged download, not a tampered release.

## Testing and reporting problems

This is a pre-release. **It is expected that you play it and test it on your own machine**, and that anything that does
not work is reported as a [GitHub issue](https://github.com/jillsandwich1998/reGH2/issues/new/choose). A good report lets us
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
   off): only the game's own picture, never your desktop or other windows. Both are packed into one `.zip` (`report-<date>.zip`): attach that. Your home folder is replaced by `~`; open the file and check it if you want before sharing it.
4. Include your graphics card, its driver and your Linux distribution if the report cannot tell them (it usually can).

For a problem you can *see* (a wrong picture), press **F9** while it is on screen and attach the PNG as well.

## Known limits

* Tested on one machine and one graphics card. NVIDIA, Intel, Steam Deck / SteamOS and other distributions are untested; reports are welcome.
* Not measured: the time from pressing a fret to the sound and to the picture (the audio buffers are small, see *Tools for finding performance problems*).
* The graphics memory grows as new venues and songs are loaded (textures are kept, not freed): about 1.0 GB at 4K after four songs on the test machine.
* A short pause when a song loads, between the song screens, is part of how the game itself loads.
* Tested in Quick Play and in Career up to the fourth venue. The later venues, Practice, co-op and DLC songs have not been played and need play-testing.
* Windows is not supported yet.

## Requirements

Linux x86-64 (glibc 2.35 or newer; SteamOS / Steam Deck included), a Vulkan 1.2 GPU with a current Mesa or NVIDIA driver,
about 4 GB of free disk space for the build and your game files, and a few GB of RAM while compiling. While playing, the game process
used about 0.7 GB of RAM and 0.4 to 1.2 GB of graphics memory on the test machine, depending on the resolution.

The AppImage cannot carry the Vulkan *driver* for your GPU, which is part of your system. If none is found, the launcher (and `doctor` /
`launch` on the command line) says so, shows the package for your distribution and GPU, and installs it only if you agree, asking for
your administrator password:

| Distribution family | AMD / Intel | NVIDIA |
|---|---|---|
| Debian, Ubuntu, Mint, ... | `mesa-vulkan-drivers` | the NVIDIA driver from your distribution |
| Fedora, RHEL, ... | `mesa-vulkan-drivers` | the NVIDIA driver (e.g. from RPM Fusion) |
| Arch, CachyOS, Manjaro, ... | `vulkan-radeon` / `vulkan-intel` | `nvidia-utils` |
| openSUSE | `libvulkan_radeon` / `libvulkan_intel` | the NVIDIA driver |

The game also needs a working audio server (PipeWire, PulseAudio or ALSA) and a display (Wayland or X11), as any desktop has.

## Saves and uninstalling

* Your save game, achievements and installed DLC live in `~/.local/share/gh2`. In the game, **F2 > GAME > Open saves folder** opens it; in
  the launcher, **Open saves folder** does the same and **Export saves...** copies the save game and achievements into a `.zip`
  (`reGH2-x86_64.AppImage saves --export <folder>` from the command line). The DLC, which can be installed again, is not in the zip.
* **Uninstall...** in the launcher (or `reGH2-x86_64.AppImage uninstall`) removes the installed game, its shortcuts and its cache. It keeps
  your saves, DLC and achievements unless you say otherwise, and it never touches your own copy of the game. Close the game first.
* DLC (Xbox Live LIVE/CON packages) can be installed from the in-game menu, one by one or from a whole folder.

## Tools for finding performance problems

* **FPS counter:** F2 > GAME > *Show FPS counter*.
* **Slowdown log:** whenever the game itself runs below about 55 fps for a second, or one frame takes over 50 ms, a line with the
  time goes to `logs/perf.log` next to the game. The problem report includes it.
* **Audio:** the buffers are very small and grow by themselves if the sound device cannot keep up. If you hear crackling,
  start the game with `--audio_latency=balanced` (or `safe`) and tell us about your hardware.
* **Graphics:** if the picture is wrong at a high resolution, try `--msaa=on` or a lower resolution and report it with an F9 screenshot.

## Supported game version

`default.xex` with SHA-1 `b90b864487e6291761b207c8d0064e9a0779bdee` (Xbox 360, title 415607E7). Other versions are refused
unless you pass `--force`.

## Building from source

For development; players use the AppImage, which already contains everything below.

```sh
scripts/build-sdk.sh ~/gh2-sdk                           # patched ReXGlue SDK (pinned commit + sdk-patches/)
native/third_party/fetch.sh                              # XenosRecomp (patched with native/third_party/patches)
scripts/gh2-recomp --sdk ~/gh2-sdk install <your game>   # codegen + build, needs clang >= 20, cmake, ninja
scripts/gh2-recomp --sdk ~/gh2-sdk launch
```
The layout of the native runtime and renderer, and the rules that keep them apart from the SDK, are in `native/README.md` and
`native/rt/abi/`. The AppImage is built by `packaging/` (Ubuntu 22.04 container, so it runs on older distributions as well).

## License

MIT (see `LICENSE`); third-party notices: `THIRD_PARTY_NOTICES.md`. The ReXGlue SDK, XenosRecomp, SDL3, Dear ImGui and the other
components keep their own licences.

`packaging/icon.svg` (and the rendered `icon.png`) is original artwork made for this project and covered by the same MIT licence.
