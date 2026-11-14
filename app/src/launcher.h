// The launcher: the first screen a player sees (gh2 --launcher; the AppImage opens it when started without arguments). It installs the game
// from the player's own copy, changes the settings, checks for updates and starts the game. It runs without the game code (the host alone),
// so it works before the game module has been built: the window is the SDK's, the picture is the native renderer in overlay-only mode, the
// screens are Dear ImGui, and every task (install, update, report, launch) is the installer's command line (scripts/gh2install), run as a
// child process whose "[ nn%] stage" lines drive the progress screen.
#pragma once

#include <memory>

namespace rex::ui {
class WindowedApp;
class WindowedAppContext;
}  // namespace rex::ui

// True when the player asked for the launcher (--launcher, or GH2_LAUNCHER=1).
bool Gh2LauncherRequested();
std::unique_ptr<rex::ui::WindowedApp> Gh2CreateLauncherApp(rex::ui::WindowedAppContext& context);
