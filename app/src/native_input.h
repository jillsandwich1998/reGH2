// The "native" controller reader: the SDK's InputSystem keeps the policy (which device plays which player, merging, dead zones, the
// UI blockers of guest dialogs, the logo-skip watcher), but the devices are read by gh2plat's dedicated SDL3 thread
// (native/platform/input_sdl3) instead of the SDK's SDL driver, whose state is refreshed by the UI thread on every guest poll.
// `native` is the default (approved after the physical X-plorer test, 2026-10-24); `--input_reader=sdk` (REX_INPUT_READER=sdk, or input_reader = "sdk"
// in the config file) brings the SDK's SDL driver back.
#pragma once

namespace rex {
struct RuntimeConfig;
}

bool Gh2NativeInputSelected();
// Call from OnPreSetup: the input system the runtime creates gets the native driver and no SDK SDL driver.
void Gh2InstallNativeInput(rex::RuntimeConfig& config);
