// gh2plat audio frame ring: lock-free, wait-free single-producer / single-consumer queue of fixed-size frames.
//
// The producer (the guest's audio render thread) fills a slot in place and publishes it; the consumer (the SDL audio callback, or a
// software clock) reads and releases the oldest one. No allocation, no lock, no system call after construction. Each side keeps a
// private copy of the other side's index, so a push or pop touches the shared cache line only when its cached view says full/empty.
// No SDL, no SDK.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace gh2plat::audio {

class FrameRing {
 public:
  // capacity_frames must be a power of two.
  FrameRing(uint32_t capacity_frames, uint32_t floats_per_frame)
      : mask_(capacity_frames - 1), floats_(floats_per_frame), data_(new float[static_cast<size_t>(capacity_frames) * floats_per_frame]()) {}

  uint32_t capacity() const { return mask_ + 1; }
  uint32_t floats_per_frame() const { return floats_; }

  // ---- producer thread ----
  float* ProducerSlot() noexcept {  // nullptr when full
    const uint64_t h = head_.load(std::memory_order_relaxed);
    if (h - cached_tail_ > mask_) {
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (h - cached_tail_ > mask_) return nullptr;
    }
    return &data_[static_cast<size_t>(h & mask_) * floats_];
  }
  void ProducerCommit() noexcept { head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

  // ---- consumer thread ----
  const float* ConsumerPeek() noexcept {  // nullptr when empty
    const uint64_t t = tail_.load(std::memory_order_relaxed);
    if (t == cached_head_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (t == cached_head_) return nullptr;
    }
    return &data_[static_cast<size_t>(t & mask_) * floats_];
  }
  void ConsumerPop() noexcept { tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

  // any thread
  uint32_t SizeApprox() const noexcept {
    const uint64_t t = tail_.load(std::memory_order_acquire);
    const uint64_t h = head_.load(std::memory_order_acquire);
    return static_cast<uint32_t>(h - t);
  }

 private:
  const uint32_t mask_;
  const uint32_t floats_;
  std::unique_ptr<float[]> data_;
  alignas(64) std::atomic<uint64_t> head_{0};  // next slot to fill (producer writes)
  alignas(64) std::atomic<uint64_t> tail_{0};  // next slot to read (consumer writes)
  alignas(64) uint64_t cached_tail_ = 0;       // producer's view of tail_
  alignas(64) uint64_t cached_head_ = 0;       // consumer's view of head_
};

}  // namespace gh2plat::audio
