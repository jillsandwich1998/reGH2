#include "native_window.h"

#include <SDL3/SDL.h>
#include <rex/ui/window.h>

#include "native_overlay.h"

#include <atomic>
#include <cstdio>

namespace {
std::atomic<uint32_t> g_width{0}, g_height{0};
gh2rt::NativeWindow g_window;
std::atomic<bool> g_ready{false};
}  // namespace

void Gh2NativeWindowResized(uint32_t width, uint32_t height) {
  g_width.store(width);
  g_height.store(height);
  Gh2RenderOptionsWindowChanged();
}

// The SDK's window is a plain SDL window (the SDK's own presenter reaches its surface through the same SDL properties); the application has exactly one.
void Gh2NativeWindowOpened(rex::ui::Window* window) {
  if (g_ready.load()) return;
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* sdl = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  if (!sdl) {
    std::fprintf(stderr, "NATIVE WINDOW: no SDL window yet\n");
    return;
  }
  const SDL_PropertiesID props = SDL_GetWindowProperties(sdl);
  gh2rt::NativeWindow w;
  if (void* display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr)) {
    w.kind = gh2rt::NativeWindow::Kind::kWayland;
    w.display = display;
    w.surface = reinterpret_cast<uint64_t>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
  } else if (void* xdisplay = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr)) {
    w.kind = gh2rt::NativeWindow::Kind::kXlib;
    w.display = xdisplay;
    w.surface = static_cast<uint64_t>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  } else {
    std::fprintf(stderr, "NATIVE WINDOW: the SDL window has neither a Wayland nor an X11 surface\n");
    return;
  }
  int pw = 0, ph = 0;
  SDL_GetWindowSizeInPixels(sdl, &pw, &ph);
  g_width.store(window && window->GetActualPhysicalWidth() ? window->GetActualPhysicalWidth() : static_cast<uint32_t>(pw));
  g_height.store(window && window->GetActualPhysicalHeight() ? window->GetActualPhysicalHeight() : static_cast<uint32_t>(ph));
  w.width = &g_width;
  w.height = &g_height;
  g_window = w;
  g_ready.store(true, std::memory_order_release);
  std::fprintf(stderr, "NATIVE WINDOW: %s surface, %ux%u\n", w.kind == gh2rt::NativeWindow::Kind::kWayland ? "Wayland" : "X11", g_width.load(), g_height.load());
}

bool Gh2GetNativeWindow(gh2rt::NativeWindow* out) {
  if (!g_ready.load(std::memory_order_acquire)) return false;
  *out = g_window;
  return true;
}

uint32_t Gh2NativeWindowPixelWidth() { return g_width.load(); }
uint32_t Gh2NativeWindowPixelHeight() { return g_height.load(); }
