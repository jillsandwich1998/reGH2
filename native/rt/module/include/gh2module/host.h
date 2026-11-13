// gh2module/host.h - what the application calls to bring in the game module (native/rt/module/host_side.cpp). Free of shim and SDK headers,
// so the application (which sees the SDK) can include it.
#pragma once

namespace gh2module {

// Loads the game module (gh2game.dll next to the executable, or the file in GH2_GAME_MODULE), completes the handshake and returns the game's
// image info: a `const rex::PPCImageInfo*` (the generated PPCImageConfig). Call before ReXApp is constructed. On any failure it tells the
// player (a message box on Windows) and exits; it never returns null. Later calls return the same pointer.
const void* OpenGameModule();

}  // namespace gh2module
