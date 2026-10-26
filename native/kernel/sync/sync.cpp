#include "gh2k/sync.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace gh2k::sync {

namespace {

inline uint32_t Be32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
inline void PutBe32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}
// A big endian guest word read and written with atomic semantics (relaxed on its own: the ordering comes from the lock count).
inline std::atomic<uint32_t>& Word(uint8_t* base, uint32_t a) { return *reinterpret_cast<std::atomic<uint32_t>*>(base + a); }
inline std::atomic<int32_t>& Count(uint8_t* base, uint32_t a) { return *reinterpret_cast<std::atomic<int32_t>*>(base + a); }

constexpr uint32_t kCsLockCount = 0x10, kCsRecursion = 0x14, kCsOwner = 0x18, kCsSpinByte = 1;

// The owner word is read by threads that do not own the section (to see whether it is themselves): atomic, relaxed.
inline uint32_t OwnerOf(uint8_t* base, uint32_t cs) { return __builtin_bswap32(Word(base, cs + kCsOwner).load(std::memory_order_relaxed)); }
inline void SetOwner(uint8_t* base, uint32_t cs, uint32_t thread) { Word(base, cs + kCsOwner).store(__builtin_bswap32(thread), std::memory_order_relaxed); }


inline uint32_t CurrentThread(uint8_t* base, uint32_t pcr) { return Be32(base, pcr + kPcrCurrentThread); }

std::atomic<uint64_t> g_contended{0}, g_spin_waits{0};

// parking lot: 64 buckets, each a mutex + condition variable + wake tokens per address
struct Bucket {
  std::mutex m;
  std::condition_variable cv;
  std::unordered_map<uint32_t, uint32_t> tokens;
};
Bucket g_buckets[64];
Bucket& BucketOf(uint32_t address) { return g_buckets[(address >> 3) & 63]; }

void Park(uint32_t address) {
  Bucket& b = BucketOf(address);
  std::unique_lock<std::mutex> lock(b.m);
  b.cv.wait(lock, [&] { return b.tokens[address] > 0; });
  if (--b.tokens[address] == 0) b.tokens.erase(address);
}

void Wake(uint32_t address) {
  Bucket& b = BucketOf(address);
  {
    std::lock_guard<std::mutex> lock(b.m);
    ++b.tokens[address];
  }
  b.cv.notify_all();
}

}  // namespace

uint32_t RaiseIrql(uint8_t* base, uint32_t pcr, uint32_t new_irql) {
  const uint32_t old = base[pcr + kPcrCurrentIrql];
  base[pcr + kPcrCurrentIrql] = static_cast<uint8_t>(new_irql);
  return old;
}

void LowerIrql(uint8_t* base, uint32_t pcr, uint32_t old_irql) { base[pcr + kPcrCurrentIrql] = static_cast<uint8_t>(old_irql); }

void SpinAcquire(uint8_t* base, uint32_t pcr, uint32_t lock) {
  const uint32_t self = __builtin_bswap32(pcr);
  std::atomic<uint32_t>& w = Word(base, lock);
  bool waited = false;
  for (;;) {
    uint32_t expected = 0;
    if (w.compare_exchange_weak(expected, self, std::memory_order_acquire, std::memory_order_relaxed)) break;
    waited = true;
    std::this_thread::yield();
  }
  if (waited) ++g_spin_waits;
}

bool SpinTryAcquire(uint8_t* base, uint32_t pcr, uint32_t lock) {
  uint32_t expected = 0;
  return Word(base, lock).compare_exchange_strong(expected, __builtin_bswap32(pcr), std::memory_order_acquire);
}

void SpinRelease(uint8_t* base, uint32_t lock) { Word(base, lock).store(0, std::memory_order_release); }

uint32_t SpinAcquireRaising(uint8_t* base, uint32_t pcr, uint32_t lock) {
  const uint32_t old = RaiseIrql(base, pcr, kIrqlDispatch);
  SpinAcquire(base, pcr, lock);
  return old;
}

void SpinReleaseLowering(uint8_t* base, uint32_t pcr, uint32_t lock, uint32_t old_irql) {
  SpinRelease(base, lock);
  if (old_irql < kIrqlDispatch) LowerIrql(base, pcr, old_irql);
}

void CritInit(uint8_t* base, uint32_t cs, uint32_t spin_count) {
  std::memset(base + cs, 0, 0x1C);
  base[cs] = 1;   // EventSynchronizationObject: the contended path sleeps on it
  uint32_t div = (spin_count + 255) >> 8;
  if (div > 255) div = 255;
  base[cs + kCsSpinByte] = static_cast<uint8_t>(div);
  Count(base, cs + kCsLockCount).store(-1, std::memory_order_relaxed);
}

void CritEnter(uint8_t* base, uint32_t pcr, uint32_t cs) {
  const uint32_t me = CurrentThread(base, pcr);
  std::atomic<int32_t>& count = Count(base, cs + kCsLockCount);
  if (OwnerOf(base, cs) == me) {   // recursive: only the owner can see its own id there
    count.fetch_add(1, std::memory_order_relaxed);
    PutBe32(base, cs + kCsRecursion, Be32(base, cs + kCsRecursion) + 1);
    return;
  }
  for (uint32_t spins = base[cs + kCsSpinByte] * 256u; spins > 0; --spins) {
    int32_t expected = -1;
    if (count.compare_exchange_weak(expected, 0, std::memory_order_acquire, std::memory_order_relaxed)) {
      SetOwner(base, cs, me);
      PutBe32(base, cs + kCsRecursion, 1);
      return;
    }
  }
  if (count.fetch_add(1, std::memory_order_acquire) != -1) {   // somebody holds it: sleep until a Leave hands it over
    ++g_contended;
    Park(cs);
  }
  SetOwner(base, cs, me);
  PutBe32(base, cs + kCsRecursion, 1);
}

bool CritTryEnter(uint8_t* base, uint32_t pcr, uint32_t cs) {
  const uint32_t me = CurrentThread(base, pcr);
  std::atomic<int32_t>& count = Count(base, cs + kCsLockCount);
  int32_t expected = -1;
  if (count.compare_exchange_strong(expected, 0, std::memory_order_acquire)) {
    SetOwner(base, cs, me);
    PutBe32(base, cs + kCsRecursion, 1);
    return true;
  }
  if (OwnerOf(base, cs) == me) {
    count.fetch_add(1, std::memory_order_relaxed);
    PutBe32(base, cs + kCsRecursion, Be32(base, cs + kCsRecursion) + 1);
    return true;
  }
  return false;
}

void CritLeave(uint8_t* base, uint32_t pcr, uint32_t cs) {
  (void)pcr;
  std::atomic<int32_t>& count = Count(base, cs + kCsLockCount);
  const uint32_t recursion = Be32(base, cs + kCsRecursion) - 1;
  PutBe32(base, cs + kCsRecursion, recursion);
  if (recursion != 0) {   // still held by this thread
    count.fetch_sub(1, std::memory_order_relaxed);
    return;
  }
  SetOwner(base, cs, 0);
  if (count.fetch_sub(1, std::memory_order_release) != 0) Wake(cs);   // there were waiters
}

uint32_t TlsGet(uint8_t* base, uint32_t pcr, uint32_t index) {
  const uint32_t slots = Be32(base, CurrentThread(base, pcr) + kThreadTlsSlots);
  return Be32(base, slots + 4 * index);
}

void TlsSet(uint8_t* base, uint32_t pcr, uint32_t index, uint32_t value) {
  const uint32_t slots = Be32(base, CurrentThread(base, pcr) + kThreadTlsSlots);
  PutBe32(base, slots + 4 * index, value);
}

Counters Stats() { return {g_contended.load(), g_spin_waits.load()}; }

}  // namespace gh2k::sync
