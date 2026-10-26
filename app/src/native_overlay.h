// The in-game menu for --gpu=native: the SDK's Dear ImGui drawer records its draw lists into an ImmediateDrawer of ours, which hands them to the native renderer to draw over the picture.
// (With the SDK's own graphics system the SDK's presenter does all this.)
#pragma once

#include <cstdint>
#include <memory>

namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
class WindowedAppContext;
}  // namespace rex::ui

// ReXApp::OnCreateImmediateDrawer in detached mode: the drawer that the ImGui drawer is given.
std::unique_ptr<rex::ui::ImmediateDrawer> Gh2CreateNativeOverlayDrawer();
// After the dialogs exist: from now on the menu's frame is drawn once per presented game frame.
void Gh2NativeOverlayStart(rex::ui::ImGuiDrawer* drawer, rex::ui::WindowedAppContext* context);

// The renderer options the menu owns (render_resolution, msaa, anisotropic_override and, for "auto", the window's size) are pushed to the native renderer, which applies them between two frames.
void Gh2ApplyRenderOptions();
// The window changed size: with render_resolution=auto the renderer follows, a moment after the resizing stops. Any thread.
void Gh2RenderOptionsWindowChanged();
// Shutdown: the renderer stops asking for menu frames.
void Gh2NativeOverlayStop();
// The screenshot key (F9, registered by the menu): saves the next picture, as the game draws it, to ~/gh2_shots.
void Gh2NativeScreenshot();
