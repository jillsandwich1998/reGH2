#include "gh2k/objects.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace gh2k::sync {

namespace {

inline int32_t State(const uint8_t* base, uint32_t o) {
  uint32_t v;
  std::memcpy(&v, base + o + 4, 4);
  return static_cast<int32_t>(__builtin_bswap32(v));
}
inline void SetState(uint8_t* base, uint32_t o, int32_t s) {
  const uint32_t v = __builtin_bswap32(static_cast<uint32_t>(s));
  std::memcpy(base + o + 4, &v, 4);
}
inline int32_t Limit(const uint8_t* base, uint32_t o) {
  uint32_t v;
  std::memcpy(&v, base + o + 0x10, 4);
  return static_cast<int32_t>(__builtin_bswap32(v));
}

// never destroyed: at exit native threads may still be inside a wait, and a destroyed mutex / condition variable under them is worse than a leak
std::mutex& g_lock = *new std::mutex;
std::condition_variable& g_changed = *new std::condition_variable;
std::atomic<uint64_t> g_sets{0}, g_resets{0}, g_releases{0}, g_waits{0}, g_blocked{0}, g_timeouts{0}, g_multi{0};

bool Satisfied(const uint8_t* base, uint32_t o) { return State(base, o) > 0 || (base[o] != kTypeSemaphore && State(base, o) != 0); }

// The wait is over for this object: take what it gives (a synchronization event clears, a semaphore loses one)
void Consume(uint8_t* base, uint32_t o) {
  if (base[o] == kTypeSynchronizationEvent) SetState(base, o, 0);
  else if (base[o] == kTypeSemaphore) SetState(base, o, State(base, o) - 1);
}

std::mutex& g_threads_lock = *new std::mutex;
std::unordered_set<uint32_t>& g_threads = *new std::unordered_set<uint32_t>;

}  // namespace

bool IsRegisteredThread(uint32_t kthread) {
  std::lock_guard<std::mutex> lock(g_threads_lock);
  return g_threads.count(kthread) != 0;
}
void ThreadRegister(uint32_t kthread) {
  std::lock_guard<std::mutex> lock(g_threads_lock);
  g_threads.insert(kthread);
}
void ThreadUnregister(uint32_t kthread) {
  std::lock_guard<std::mutex> lock(g_threads_lock);
  g_threads.erase(kthread);
}
bool IsNative(const uint8_t* base, uint32_t object) { return IsNativeType(base[object]) || (base[object] == kTypeThread && IsRegisteredThread(object)); }
void ThreadExited(uint8_t* base, uint32_t kthread) {
  {
    std::lock_guard<std::mutex> lock(g_lock);
    SetState(base, kthread, 1);
  }
  g_changed.notify_all();
}

void InitEvent(uint8_t* base, uint32_t event, uint8_t type, bool signalled) {
  std::lock_guard<std::mutex> lock(g_lock);
  std::memset(base + event, 0, 0x10);
  base[event] = type;
  base[event + 2] = 4;   // size in dwords
  SetState(base, event, signalled ? 1 : 0);
}

void InitSemaphore(uint8_t* base, uint32_t sem, int32_t count, int32_t limit) {
  std::lock_guard<std::mutex> lock(g_lock);
  std::memset(base + sem, 0, 0x14);
  base[sem] = kTypeSemaphore;
  base[sem + 2] = 5;
  SetState(base, sem, count);
  const uint32_t v = __builtin_bswap32(static_cast<uint32_t>(limit));
  std::memcpy(base + sem + 0x10, &v, 4);
}

int32_t EventSet(uint8_t* base, uint32_t event) {
  int32_t previous;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    previous = State(base, event);
    SetState(base, event, 1);
  }
  ++g_sets;
  if (previous == 0) g_changed.notify_all();
  return previous;
}

int32_t EventReset(uint8_t* base, uint32_t event) {
  std::lock_guard<std::mutex> lock(g_lock);
  const int32_t previous = State(base, event);
  SetState(base, event, 0);
  ++g_resets;
  return previous;
}

uint32_t SemaphoreRelease(uint8_t* base, uint32_t sem, int32_t adjustment, int32_t* previous) {
  {
    std::lock_guard<std::mutex> lock(g_lock);
    const int32_t before = State(base, sem);
    if (previous) *previous = before;
    if (adjustment < 0 || static_cast<int64_t>(before) + adjustment > Limit(base, sem)) return kStatusSemaphoreLimitExceeded;
    SetState(base, sem, before + adjustment);
  }
  ++g_releases;
  if (adjustment > 0) g_changed.notify_all();
  return kStatusSuccess;
}

uint32_t Wait(uint8_t* base, const uint32_t* objects, uint32_t count, bool wait_all, uint64_t timeout_100ns) {
  ++g_waits;
  if (count > 1) ++g_multi;
  const bool infinite = timeout_100ns == kInfinite;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(infinite ? 0 : timeout_100ns * 100);
  std::unique_lock<std::mutex> lock(g_lock);
  bool blocked = false;
  for (;;) {
    if (wait_all) {
      bool all = true;
      for (uint32_t i = 0; i < count && all; ++i) all = Satisfied(base, objects[i]);
      if (all) {
        for (uint32_t i = 0; i < count; ++i) Consume(base, objects[i]);
        return kStatusSuccess;
      }
    } else {
      for (uint32_t i = 0; i < count; ++i)
        if (Satisfied(base, objects[i])) {
          Consume(base, objects[i]);
          return i;
        }
    }
    if (!infinite && timeout_100ns == 0) {
      ++g_timeouts;
      return kStatusTimeout;
    }
    if (!blocked) {
      blocked = true;
      ++g_blocked;
    }
    if (infinite) {
      g_changed.wait(lock);
    } else if (g_changed.wait_until(lock, deadline) == std::cv_status::timeout) {
      // one last look: the deadline and a signal can coincide
      bool any = false;
      for (uint32_t i = 0; i < count; ++i) any |= Satisfied(base, objects[i]);
      if (!any || wait_all) {
        ++g_timeouts;
        return kStatusTimeout;
      }
    }
  }
}

uint64_t TimeoutFromGuest(const uint8_t* base, uint32_t timeout_ptr) {
  if (!timeout_ptr) return kInfinite;
  uint64_t v;
  std::memcpy(&v, base + timeout_ptr, 8);
  const int64_t t = static_cast<int64_t>(__builtin_bswap64(v));
  if (t == 0) return 0;
  if (t < 0) return static_cast<uint64_t>(-t);
  // absolute FILETIME (100 ns since 1601)
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count() / 100 + 116444736000000000ll;
  return t > now ? static_cast<uint64_t>(t - now) : 0;
}

ObjectCounters ObjectStats() { return {g_sets.load(), g_resets.load(), g_releases.load(), g_waits.load(), g_blocked.load(), g_timeouts.load(), g_multi.load()}; }

}  // namespace gh2k::sync
