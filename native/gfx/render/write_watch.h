// Tells the texture cache which guest pages were written, so that it does not have to re-read every texture every frame. A page a texture uses is made read-only in the windows
// the guest writes through; the first write faults, the handler records it and gives the page back. The renderer reads through the physical view, which stays writable.
// Linux x86-64. Writes through the physical view (the command processor's) and protection the runtime changes behind our back are not seen: the cache still hashes every texture
// now and then. GH2_TEX_WATCH=0 turns all of this off.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gh2render {

class WriteWatch {
 public:
  struct Stats {
    uint64_t faults, touches, armed_pages, arm_failures;
  };

  // Once, with the host address of guest address 0. False when it is turned off or the handler cannot be installed.
  static bool Start(uint8_t* guest_base);
  static bool Active();

  // The value to compare with Written(). Take it before Arm and before reading the bytes: a write that lands after it is reported.
  static uint32_t Epoch();
  // Makes the pages of [physical, physical + bytes) read-only. False when the kernel refuses: nothing can be said about these pages.
  static bool Arm(uint32_t physical, uint64_t bytes);
  // Was any page of the range written after `epoch`?
  static bool Written(uint32_t physical, uint64_t bytes, uint32_t epoch);

  static Stats GetStats();
};

}  // namespace gh2render
