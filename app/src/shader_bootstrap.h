// The translated shaders (shaders.pak) the native renderer needs are made from the player's own game. When the file is missing or from an older format, the game makes it itself at its
// first start (about 15 seconds, once): native/tools/shader_pipeline.py runs as a child process while the game boots, a dialog shows the progress, and the renderer opens the cache the
// moment it is ready (nothing can be drawn until then).
#pragma once

#include <string>

namespace rex::ui {
class ImGuiDrawer;
}

// True when `dir`/shaders.pak exists and is in the format this build reads.
bool Gh2ShaderCacheCurrent(const std::string& dir);
// Called once, early: when `dir` has no current shaders.pak and it can be made (tools, python3, the game's gen/ and the XenosRecomp binary are all there, the option allows it and the last
// attempt did not fail), starts making it in the background and returns true. False: nothing to wait for (the cache is current, or the compatibility renderer must be used).
bool Gh2ShaderBuildBegin(const std::string& dir);
// The game's executable is in guest memory (OnPreLaunchModule, the runtime has loaded and decoded it): starts the planned build, handing it a copy of the image (the three shaders of the
// intro video are inside the executable). `guest_base` may be null: the build then goes without them.
void Gh2ShaderBuildStart(const unsigned char* guest_base);
// After the dialogs exist: the progress dialog (only when a build was started).
void Gh2ShaderBuildShowDialog(rex::ui::ImGuiDrawer* drawer);
