// gh2plat XMA port: the seam where a native XMA decoder will plug in. INTERFACE ONLY - nothing implements it yet (Phase 2, audio step).
//
// What the Quick Play trace shows (native/platform/README.md has the numbers): the title drives the console's XMA unit through
// XMACreateContext / XMAReleaseContext (25 / 23 calls) and then ONLY through guest memory and the XMA registers at 0x7FEA0000:
// ContextArrayAddress (+0x1800, read once), CurrentContextIndex (+0x1818, polled), Kick (+0x1940.., written), Lock (+0x1A40..) and
// Clear (+0x1A80..). The imports of the contract cover only the allocation; everything else reaches the host as MMIO pseudo events.
//
// What that means for routing: the unit decodes XMA2 packets into 16-bit PCM that the *title* reads back from guest memory; the
// title's own software mixer then submits the final 6 x 256 float frames through XAudioSubmitRenderDriverFrame, which is what the
// AudioSink plays. So XMA never goes to the SDL stream directly: the decoder writes PCM into the guest's output ring, and the mixed
// result reaches the sink the way it does today.
#pragma once

#include <cstdint>

namespace gh2plat::audio {

// The decoder's view of the guest: translate a guest address to a host pointer (the guest's memory is one flat mapping).
class XmaGuestMemory {
 public:
  virtual ~XmaGuestMemory() = default;
  virtual uint8_t* Translate(uint32_t guest_address) = 0;
};

// A native XMA unit. The runtime forwards the title's MMIO accesses and the two imports; the unit decodes on its own thread(s) and
// writes PCM to the guest's output buffers. Calls arrive on the guest's threads: implementations must be thread-safe.
class XmaUnit {
 public:
  virtual ~XmaUnit() = default;

  // XMACreateContext / XMAReleaseContext: returns the guest address of the 64-byte context record, 0 when none is free.
  virtual uint32_t AllocateContext() = 0;
  virtual void ReleaseContext(uint32_t context_guest_address) = 0;

  // MMIO at 0x7FEA0000 + byte_offset (32-bit accesses, guest byte order already resolved by the caller).
  virtual uint32_t ReadRegister(uint32_t byte_offset) = 0;
  virtual void WriteRegister(uint32_t byte_offset, uint32_t value) = 0;
};

constexpr uint32_t kXmaMmioBase = 0x7FEA0000;
constexpr uint32_t kXmaMmioSize = 0x10000;
constexpr uint32_t kXmaContextCount = 320;  // hardware contexts (as emulated by the SDK today)
// Register groups seen in the Quick Play trace (byte offsets; word index * 4)
constexpr uint32_t kXmaRegContextArrayAddress = 0x1800;
constexpr uint32_t kXmaRegCurrentContextIndex = 0x1818;
constexpr uint32_t kXmaRegKick = 0x1940;   // + 4 * group (10 groups of 32 contexts); hypothesis: writing a bit mask starts those contexts
constexpr uint32_t kXmaRegLock = 0x1A40;   // names and strides from the SDK's register table; their semantics are not verified here
constexpr uint32_t kXmaRegClear = 0x1A80;

}  // namespace gh2plat::audio
