// gh2plat snapshot slot: one device's latest state, published by the polling thread and read by the guest's input thread.
//
// A seqlock over relaxed atomics: the single writer never waits and never blocks on a reader, readers retry only when they hit a
// write in progress, and no read can observe a torn state (checked by gh2_input_selftest's stress test). No SDL, no SDK.
#pragma once

#include <atomic>
#include <cstdint>

namespace gh2plat {

// XInput gamepad state in host byte order (same layout of fields as XINPUT_GAMEPAD: buttons, triggers, four stick axes).
struct PadState {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool operator==(const PadState&) const = default;
};

struct Sample {
  PadState pad;
  uint32_t changes = 0;        // how many distinct states the thread has observed on this device (0 = nothing yet)
  uint64_t first_seen_ns = 0;  // steady_clock ns at which `pad` was first observed by the polling thread
};

class SnapshotSlot {
 public:
  void Write(const Sample& s) noexcept {  // single writer
    const uint32_t q = seq_.load(std::memory_order_relaxed);
    seq_.store(q + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    w_[0].store(static_cast<uint64_t>(s.pad.buttons) | static_cast<uint64_t>(s.pad.left_trigger) << 16 |
                    static_cast<uint64_t>(s.pad.right_trigger) << 24 | static_cast<uint64_t>(static_cast<uint16_t>(s.pad.lx)) << 32 |
                    static_cast<uint64_t>(static_cast<uint16_t>(s.pad.ly)) << 48,
                std::memory_order_relaxed);
    w_[1].store(static_cast<uint64_t>(static_cast<uint16_t>(s.pad.rx)) | static_cast<uint64_t>(static_cast<uint16_t>(s.pad.ry)) << 16 |
                    static_cast<uint64_t>(s.changes) << 32,
                std::memory_order_relaxed);
    w_[2].store(s.first_seen_ns, std::memory_order_relaxed);
    seq_.store(q + 2, std::memory_order_release);
  }

  Sample Read() const noexcept {  // any thread
    for (;;) {
      const uint32_t s1 = seq_.load(std::memory_order_acquire);
      if (s1 & 1u) continue;
      const uint64_t a = w_[0].load(std::memory_order_relaxed);
      const uint64_t b = w_[1].load(std::memory_order_relaxed);
      const uint64_t c = w_[2].load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (seq_.load(std::memory_order_relaxed) != s1) continue;
      Sample s;
      s.pad.buttons = static_cast<uint16_t>(a);
      s.pad.left_trigger = static_cast<uint8_t>(a >> 16);
      s.pad.right_trigger = static_cast<uint8_t>(a >> 24);
      s.pad.lx = static_cast<int16_t>(a >> 32);
      s.pad.ly = static_cast<int16_t>(a >> 48);
      s.pad.rx = static_cast<int16_t>(b);
      s.pad.ry = static_cast<int16_t>(b >> 16);
      s.changes = static_cast<uint32_t>(b >> 32);
      s.first_seen_ns = c;
      return s;
    }
  }

 private:
  std::atomic<uint32_t> seq_{0};
  std::atomic<uint64_t> w_[3] = {};
};

}  // namespace gh2plat
