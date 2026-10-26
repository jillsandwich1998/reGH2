// The guest address space comes from gh2k::mem (native/kernel/mem), not from the runtime: OnPreSetup reserves the 4 GB arena (one memfd, the views of the
// Xbox 360 layout) and tells the runtime's memory manager to lay its heaps over it (rex::memory::SetExternalArena, SDK patch 0010).
// `--arena=sdk` leaves the runtime to make its own.
#pragma once

bool Gh2NativeArenaSelected();
// Call from OnPreSetup, before the runtime creates its memory manager. Returns false (and the runtime keeps its own arena) when the reservation fails.
bool Gh2InstallNativeArena();
