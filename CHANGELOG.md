# Changelog

## Unreleased

**New**
* Windows 11 source build (clang + lld-link, Vulkan): `scripts/windows/`, `native/platform/os` (operating-system services behind one API), a Win32 window surface, Windows guest memory (placeholders and a pagefile-backed section) and file access. It reaches the title screen at 60 fps. See *Building on Windows* in the README.
* Windows installer and launcher: `reGH2-Setup-<version>.exe` (Inno Setup, per-user, no administrator rights) installs the launcher, the game's host prebuilt (`host\`: `gh2.exe` and the DLLs it loads, made by `gh2win.py make-host`) and the tools that build the rest (llvm-mingw, Ninja, Python with Tk, the SDK's code generator, the shader tools); the launcher translates the player's own `default.xex` and compiles only the game module `gh2game.dll` (`scripts/gh2install/win.py`), with nothing downloaded and nothing from Microsoft to install; it checks for a Vulkan driver; the install shows the compile live (step, file, elapsed time, estimate, the compiler's output); and it updates itself from the release's `Setup.exe` (checked against `SHA256SUMS.txt`). Built by `packaging/windows/make-installer.ps1`. A development checkout without a prebuilt host still builds everything (`gh2win.py build --full`, CMake + Visual Studio's C++ tools).
* Windows game module: with `GH2_GAME_MODULE` (on by default on Windows) the generated game code is built as `gh2game.dll` with llvm-mingw (clang, lld, libc++ and mingw-w64, all redistributable) and loaded by `gh2.exe` at start. Only plain C crosses between the two: the runtime services, the 179 kernel imports, the 916 game functions the hooks replace and their original code, checked by a handshake (contract version, `PPCContext` layout, the hash of both lists). See `native/rt/module`, `native/tools/gen_module_stubs.py` and `native/tools/build_game_module.py`. Linux is unchanged (the option is off there).
* SDK patches 0012 (an exported setter for the runtime's current thread) and 0013 (one shared SDL3 library) for the Windows build; they apply on Linux too.
* `.gitattributes`: patches and shell scripts stay LF in Windows checkouts.

**Fixed**
* Linux: stutter when a menu screen loads (frames of up to 0.15 s). Every guest allocation that committed memory inside an existing reservation made the runtime read and parse all of /proc/self/maps to check the range; SDK patch 0014 checks it one mapping at a time with the PROCMAP_QUERY lookup (or the protection table on kernels older than 6.11). Measured on the autostart run: longest menu frame 151-159 ms -> 46-56 ms, the 61 ms frame at the start of the song is gone. Windows was never affected.
* Stutter the first time a scene is shown (the first song in a venue: frames of up to 0.8 s while the driver compiled hundreds of new pipelines one by one inside the frame). New pipelines are now compiled by a pool of threads, side by side; a frame waits for them 8 ms at most and draws the objects whose pipeline is not ready yet from the next frames on (`native/gfx/render/pipelines.cpp`; `GH2_PIPELINE_ASYNC=0` restores the old behaviour, `GH2_PIPELINE_WAIT_MS` sets the wait). The same renderer runs on Linux and Windows.
* No sound device (none plugged in, or only a monitor's that went to sleep): the game crashed a second after starting, because the audio output failed to open and the game does not survive a failed audio driver. The sink now consumes the frames with its software clock (silent, at the right speed) and looks for a device every 2 s: plugging one in (or waking the monitor) brings the sound in without a restart. A device lost while playing was already handled by SDL3 (the stream keeps time silently and follows the new default device). `GH2_TEST_AUDIO_LATE=N` pretends there is no device for N seconds (`native/platform/audio_sdl3/audio_sink.cpp`).

## 0.2.1 (pre-release)

**Fixed**
* The application-menu entry and the desktop icon started the downloaded AppImage, so deleting it (or moving it away) stopped the game from opening. The installer now keeps a copy of the AppImage in the install folder (`~/.local/share/gh2-recomp/reGH2-x86_64.AppImage`, 231 MB) and the shortcuts start that copy. It is refreshed when a newer AppImage is run and removed by the uninstaller. To get this on an existing install, open the new AppImage once.

## 0.2.0 (pre-release)

The game now runs on this project's own runtime and renderer instead of the emulation layers of the ReXGlue SDK.
The SDK stays as the code generator and the application shell (window, menus, saves, achievements, DLC).

**New**
* Native runtime: kernel services, memory, threads, file access and synchronisation (`native/kernel`, `native/rt`).
* Native Vulkan renderer with the game's shaders translated from your copy (XenosRecomp): internal resolution 720p to 4K, MSAA, anisotropic filtering, vertical sync, screenshots (F9).
* Native audio and controllers on SDL3, with small adaptive audio buffers.
* Guitar and gamepad navigation of the in-game options (hold START + BACK).
* The shader cache is made automatically on the first start (about 15 s, with a progress window); the AppImage carries every tool it needs.
* Fixes found by testing: broken crowd and notes with some MSAA/resolution settings, a blur artefact at 4K, star-power sparkle squares, the sun of one stage's intro drawn over the curtain, helper processes left running when the game is closed during the first-start build.

**Known limits**: see the README (one test machine; Quick Play tested most).

## 0.1.2

First pre-release, on the ReXGlue SDK's emulation runtime.
