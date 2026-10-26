// The application's window as the native renderer needs it: platform surface handles + live pixel size (see gh2rt::NativeWindow).
#pragma once

#include <gh2rt/host_services.h>

namespace rex::ui {
class Window;
}

// Call from the UI thread once the window is open: remembers it and creates the platform surface (kept alive for the process).
void Gh2NativeWindowOpened(rex::ui::Window* window);
// Window pixel size changed (UI thread).
void Gh2NativeWindowResized(uint32_t width, uint32_t height);
// Fills the handles; false while the window is not open. Safe from any thread.
bool Gh2GetNativeWindow(gh2rt::NativeWindow* out);
// The window's live size in pixels (0 until it is open). Safe from any thread.
uint32_t Gh2NativeWindowPixelWidth();
uint32_t Gh2NativeWindowPixelHeight();
