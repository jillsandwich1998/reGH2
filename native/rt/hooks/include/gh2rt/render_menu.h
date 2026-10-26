// gh2rt/render_menu.h - what the application's in-game menu needs from the native renderer: it draws the menu's draw lists over the picture and changes the render options while the
// game runs. The types are the renderer's own (plain C++17 + Vulkan headers; no SDK). Everything is safe to call from any thread; without --gpu=native the functions do nothing.
// This header must stay free of SDK headers and of the `rex::` namespace (native/tools/check_isolation.sh).
#pragma once

#include <gh2render/renderer.h>

#include <functional>
#include <memory>

namespace gh2rt::gpu {

// Called on the command processor's thread after every swap with the window's size in pixels; the application schedules the menu's frame on its UI thread. Set before the renderer exists.
void SetPresentCallback(std::function<void(uint32_t width, uint32_t height)> callback);
// The renderer once --gpu=native has created it, else null.
gh2render::Renderer* ActiveRenderer();

}  // namespace gh2rt::gpu
