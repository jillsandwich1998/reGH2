# Changelog

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
