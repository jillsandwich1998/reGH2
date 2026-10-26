// gh2k/objects.h - events, semaphores and waits, natively (Phase 2.7, K.2): the dispatcher objects of the Xbox 360 kernel on guest memory, and the host side of
// KeSetEvent / KeWaitForSingleObject / NtCreateEvent ... that operate on them.
//
// Objects live where the guest put them: a KEVENT / KSEMAPHORE initialised inline by the game (the title never calls KeInitializeEvent: the XDK writes the header itself)
// or allocated here for the handles NtCreateEvent / NtCreateSemaphore hand out. Layout (FACTS: read from the runtime's headers, confirmed by the types the title uses):
//   X_DISPATCH_HEADER (0x10): +0 type (0 notification event, 1 synchronization event, 5 semaphore; 6 thread and others belong to the runtime), +1 absolute, +2 size, +3 inserted,
//                             +4 signal state (be32; the count for a semaphore), +8/+C wait list (unused here)
//   KSEMAPHORE:               header, +0x10 limit (be32)
// A notification event stays signalled until reset and releases every waiter; a synchronization event releases one waiter and clears itself; a semaphore's wait takes one from the count.
// All state changes and checks happen under one lock and wake the waiters through one condition variable: with the handful of threads the title has that is simpler and faster than per-object queues.
// Plain C++17, no SDK, no guest macros.
#pragma once

#include <cstdint>

namespace gh2k::sync {

inline constexpr uint32_t kStatusSuccess = 0, kStatusTimeout = 0x102, kStatusInvalidHandle = 0xC0000008, kStatusInvalidParameter = 0xC000000D, kStatusSemaphoreLimitExceeded = 0xC0000047;
inline constexpr uint8_t kTypeNotificationEvent = 0, kTypeSynchronizationEvent = 1, kTypeSemaphore = 5;
inline constexpr uint64_t kInfinite = ~0ull;

inline constexpr uint8_t kTypeThread = 6;

// Does the native layer own objects of this header type? (false: a mutant, a timer ...: the runtime's. A thread is ours only if it was created natively: see ThreadRegister)
inline bool IsNativeType(uint8_t type) { return type == kTypeNotificationEvent || type == kTypeSynchronizationEvent || type == kTypeSemaphore; }
bool IsNative(const uint8_t* base, uint32_t object);

// Threads created natively (hooks_kthreads.cpp): the KTHREAD header is a dispatcher object (type 6) that becomes signalled, for good, when the thread ends. A wait on a thread of
// the runtime's (the main thread, its audio and GPU workers) is not ours and goes to the runtime.
void ThreadRegister(uint32_t kthread);
void ThreadUnregister(uint32_t kthread);
bool IsRegisteredThread(uint32_t kthread);
void ThreadExited(uint8_t* base, uint32_t kthread);   // signal state 1, wakes every waiter

void InitEvent(uint8_t* base, uint32_t event, uint8_t type, bool signalled);
void InitSemaphore(uint8_t* base, uint32_t semaphore, int32_t count, int32_t limit);

int32_t EventSet(uint8_t* base, uint32_t event);        // returns the previous signal state
int32_t EventReset(uint8_t* base, uint32_t event);
// returns kStatusSuccess or kStatusSemaphoreLimitExceeded; *previous gets the count before
uint32_t SemaphoreRelease(uint8_t* base, uint32_t semaphore, int32_t adjustment, int32_t* previous);

// Waits for one (any) or all of `objects` (guest addresses of dispatcher objects owned here). timeout_100ns: kInfinite, 0 (poll), or a relative time.
// Returns the NTSTATUS: index of the object that satisfied a wait-any, 0 for wait-all, kStatusTimeout.
uint32_t Wait(uint8_t* base, const uint32_t* objects, uint32_t count, bool wait_all, uint64_t timeout_100ns);

// NtWait... timeout argument: a guest pointer to a be64 (negative = relative 100 ns units, positive = absolute FILETIME, 0 pointer = infinite) -> relative time for Wait
uint64_t TimeoutFromGuest(const uint8_t* base, uint32_t timeout_ptr);

struct ObjectCounters {
  uint64_t sets, resets, releases, waits, waits_blocked, wait_timeouts, multi_waits;
};
ObjectCounters ObjectStats();

}  // namespace gh2k::sync
