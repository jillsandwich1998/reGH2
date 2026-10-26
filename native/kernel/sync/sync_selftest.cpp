// gh2_sync_selftest - the native critical sections, spin locks, IRQL and TLS slots against a synthetic guest memory and real host threads.
// Exit status 0 = everything passed. Build with -fsanitize=thread (cmake -DGH2_SYNC_TSAN=ON) to also check the memory ordering.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <atomic>

#include "gh2k/objects.h"
#include "gh2k/sync.h"

using namespace gh2k::sync;


namespace {
int g_failures = 0;
void Check(bool ok, const std::string& what, const std::string& detail = "") {
  std::printf("[%s] %s%s%s\n", ok ? "ok" : "!!", what.c_str(), detail.empty() ? "" : " - ", detail.c_str());
  if (!ok) ++g_failures;
}

std::vector<uint8_t> g_mem(1 << 20, 0);
uint8_t* Base() { return g_mem.data(); }
void Put32(uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(&g_mem[a], &v, 4);
}
uint32_t Get32(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, &g_mem[a], 4);
  return __builtin_bswap32(v);
}

constexpr uint32_t kPcrBase = 0x1000, kThreadBase = 0x20000, kTlsBase = 0x30000, kCs = 0x40000, kLock = 0x40100, kShared = 0x40200;
uint32_t Pcr(int i) { return kPcrBase + 0x400 * i; }
void MakeThread(int i) {
  const uint32_t pcr = Pcr(i), thread = kThreadBase + 0x200 * i, tls = kTlsBase + 0x100 * i;
  Put32(pcr + kPcrCurrentThread, thread);
  Put32(thread + kThreadTlsSlots, tls);
}
}  // namespace

int main() {
  for (int i = 0; i < 8; ++i) MakeThread(i);
  // IRQL
  Check(RaiseIrql(Base(), Pcr(0), kIrqlDispatch) == 0 && Base()[Pcr(0) + kPcrCurrentIrql] == 2, "raise to DISPATCH returns the old level");
  LowerIrql(Base(), Pcr(0), 0);
  Check(Base()[Pcr(0) + kPcrCurrentIrql] == 0, "lower");
  // spin lock: acquire raising, release lowering
  const uint32_t old = SpinAcquireRaising(Base(), Pcr(0), kLock);
  Check(old == 0 && Get32(kLock) == Pcr(0) && Base()[Pcr(0) + kPcrCurrentIrql] == 2, "KfAcquireSpinLock: IRQL raised, the owner is the PCR");
  Check(!SpinTryAcquire(Base(), Pcr(1), kLock), "a second CPU cannot take it");
  SpinReleaseLowering(Base(), Pcr(0), kLock, old);
  Check(Get32(kLock) == 0 && Base()[Pcr(0) + kPcrCurrentIrql] == 0 && SpinTryAcquire(Base(), Pcr(1), kLock), "released, IRQL back, now the other can take it");
  SpinRelease(Base(), kLock);
  // TLS
  TlsSet(Base(), Pcr(2), 3, 0xABCD);
  Check(TlsGet(Base(), Pcr(2), 3) == 0xABCD && TlsGet(Base(), Pcr(3), 3) == 0, "TLS slots are per thread");
  // critical section: single thread, recursion
  CritInit(Base(), kCs, 0);
  CritEnter(Base(), Pcr(0), kCs);
  CritEnter(Base(), Pcr(0), kCs);
  Check(Get32(kCs + 0x14) == 2 && Get32(kCs + 0x18) == kThreadBase, "recursive enter: count 2, owner is the KTHREAD");
  Check(!CritTryEnter(Base(), Pcr(1), kCs), "TryEnter from another thread fails");
  CritLeave(Base(), Pcr(0), kCs);
  CritLeave(Base(), Pcr(0), kCs);
  Check(Get32(kCs + 0x18) == 0 && CritTryEnter(Base(), Pcr(1), kCs), "after the last leave it is free");
  CritLeave(Base(), Pcr(1), kCs);

  // critical section: 6 threads, a counter that is only safe if the section works
  CritInit(Base(), kCs, 0);
  Put32(kShared, 0);
  constexpr int kThreads = 6, kIters = 60000;
  std::vector<std::thread> threads;
  const auto t0 = std::chrono::steady_clock::now();
  for (int t = 0; t < kThreads; ++t)
    threads.emplace_back([t] {
      for (int i = 0; i < kIters; ++i) {
        CritEnter(Base(), Pcr(t), kCs);
        if (i % 7 == 0) CritEnter(Base(), Pcr(t), kCs);   // recursion
        const uint32_t v = Get32(kShared);
        if (i % 1000 == 0) std::this_thread::yield();      // make a preemption inside the section likely
        Put32(kShared, v + 1);
        if (i % 7 == 0) CritLeave(Base(), Pcr(t), kCs);
        CritLeave(Base(), Pcr(t), kCs);
      }
    });
  for (auto& th : threads) th.join();
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  Check(Get32(kShared) == kThreads * kIters, "6 threads x 60000 critical sections: no lost update", std::to_string(Get32(kShared)) + " in " + std::to_string(static_cast<int>(ms)) + " ms, contended " +
                                                                                                     std::to_string(Stats().contended_enters));
  Check(Get32(kCs + 0x18) == 0 && static_cast<int32_t>(*reinterpret_cast<uint32_t*>(Base() + kCs + 0x10)) == -1, "left free (lock count -1, no owner)");

  // spin lock mutual exclusion
  Put32(kShared, 0);
  threads.clear();
  for (int t = 0; t < kThreads; ++t)
    threads.emplace_back([t] {
      for (int i = 0; i < kIters; ++i) {
        const uint32_t o = SpinAcquireRaising(Base(), Pcr(t), kLock);
        Put32(kShared, Get32(kShared) + 1);
        SpinReleaseLowering(Base(), Pcr(t), kLock, o);
      }
    });
  for (auto& th : threads) th.join();
  Check(Get32(kShared) == kThreads * kIters, "6 threads x 60000 spin lock sections: no lost update");

  // a sleeper is woken by the leave that hands over (no lost wake-up)
  CritInit(Base(), kCs, 0);
  CritEnter(Base(), Pcr(0), kCs);
  std::thread sleeper([] {
    CritEnter(Base(), Pcr(1), kCs);
    Put32(kShared, 777);
    CritLeave(Base(), Pcr(1), kCs);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Put32(kShared, 0);
  CritLeave(Base(), Pcr(0), kCs);
  sleeper.join();
  Check(Get32(kShared) == 777, "a thread asleep on a held section runs after the holder leaves");


  // ---- dispatcher objects ----
  constexpr uint32_t kEv = 0x50000, kEv2 = 0x50100, kSem = 0x50200, kSem2 = 0x50300;
  InitEvent(Base(), kEv, kTypeNotificationEvent, false);
  InitEvent(Base(), kEv2, kTypeSynchronizationEvent, false);
  InitSemaphore(Base(), kSem, 0, 3);
  Check(Wait(Base(), &kEv, 1, false, 0) == kStatusTimeout, "a poll of an unsignalled event times out at once");
  Check(EventSet(Base(), kEv) == 0 && EventSet(Base(), kEv) == 1, "KeSetEvent returns the previous state");
  Check(Wait(Base(), &kEv, 1, false, 0) == 0 && Wait(Base(), &kEv, 1, false, 0) == 0, "a notification event stays signalled");
  Check(EventReset(Base(), kEv) == 1 && Wait(Base(), &kEv, 1, false, 0) == kStatusTimeout, "reset clears it");
  EventSet(Base(), kEv2);
  Check(Wait(Base(), &kEv2, 1, false, 0) == 0 && Wait(Base(), &kEv2, 1, false, 0) == kStatusTimeout, "a synchronization event clears itself when a wait takes it");
  int32_t prev = -1;
  Check(SemaphoreRelease(Base(), kSem, 2, &prev) == kStatusSuccess && prev == 0, "semaphore release 2 (previous 0)");
  Check(SemaphoreRelease(Base(), kSem, 2, &prev) == kStatusSemaphoreLimitExceeded && prev == 2, "release beyond the limit is refused, previous count reported");
  Check(Wait(Base(), &kSem, 1, false, 0) == 0 && Wait(Base(), &kSem, 1, false, 0) == 0 && Wait(Base(), &kSem, 1, false, 0) == kStatusTimeout, "each wait takes one from the count");
  uint32_t both[2] = {kEv2, kSem};
  EventSet(Base(), kEv2);
  Check(Wait(Base(), both, 2, true, 0) == kStatusTimeout && Wait(Base(), both, 2, false, 0) == 0, "wait-all needs every object (the event alone does not do), wait-any returns the index");
  SemaphoreRelease(Base(), kSem, 1, &prev);
  EventSet(Base(), kEv2);
  Check(Wait(Base(), both, 2, true, 0) == 0 && Wait(Base(), both, 2, true, 0) == kStatusTimeout, "wait-all consumes all of them together");
  const auto w0 = std::chrono::steady_clock::now();
  Check(Wait(Base(), &kEv2, 1, false, 20 * 10000) == kStatusTimeout && std::chrono::steady_clock::now() - w0 >= std::chrono::milliseconds(19), "a 20 ms wait times out after 20 ms");
  // a sleeper released by a later signal; a notification event releases all sleepers
  InitEvent(Base(), kEv, kTypeNotificationEvent, false);
  std::atomic<int> woke{0};
  std::vector<std::thread> sleepers;
  for (int i = 0; i < 4; ++i) sleepers.emplace_back([&] { if (Wait(Base(), &kEv, 1, false, kInfinite) == 0) ++woke; });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  Check(woke == 0, "four sleepers are asleep on the unsignalled notification event");
  EventSet(Base(), kEv);
  for (auto& t : sleepers) t.join();
  Check(woke == 4, "one KeSetEvent releases all four");
  // producer / consumer on a semaphore + a synchronization event as the lock: nothing lost, nothing duplicated
  InitSemaphore(Base(), kSem2, 0, 0x7FFFFFFF);
  InitEvent(Base(), kEv2, kTypeSynchronizationEvent, true);   // used as a mutex
  Put32(kShared, 0);
  std::atomic<uint32_t> produced{0}, consumed{0};
  std::vector<std::thread> pc;
  for (int p = 0; p < 3; ++p) pc.emplace_back([&] { for (int i = 0; i < 20000; ++i) { SemaphoreRelease(Base(), kSem2, 1, nullptr); ++produced; } });
  for (int c = 0; c < 3; ++c)
    pc.emplace_back([&] {
      for (int i = 0; i < 20000; ++i) {
        Wait(Base(), &kSem2, 1, false, kInfinite);
        uint32_t lock = kEv2;
        Wait(Base(), &lock, 1, false, kInfinite);   // take the "mutex"
        Put32(kShared, Get32(kShared) + 1);
        EventSet(Base(), kEv2);                      // give it back
        ++consumed;
      }
    });
  for (auto& t : pc) t.join();
  Check(produced == 60000 && consumed == 60000 && Get32(kShared) == 60000, "3 producers / 3 consumers x 20000: every release is consumed once, the event-as-mutex protects the counter");
  Check(Wait(Base(), &kSem2, 1, false, 0) == kStatusTimeout, "the semaphore ends at zero");

  // ---- threads as dispatcher objects (hooks_kthreads.cpp) ----
  constexpr uint32_t kThr = 0x50400, kThrForeign = 0x50500;
  Base()[kThr] = kTypeThread;
  Base()[kThrForeign] = kTypeThread;
  Check(!IsNative(Base(), kThr), "a type 6 object nobody registered belongs to the runtime");
  ThreadRegister(kThr);
  Check(IsNative(Base(), kThr) && IsRegisteredThread(kThr) && !IsNative(Base(), kThrForeign), "a registered thread is native, another type 6 object is not");
  Check(Wait(Base(), &kThr, 1, false, 0) == kStatusTimeout, "a running thread is not signalled");
  std::atomic<int> joined{0};
  std::vector<std::thread> joiners;
  for (int i = 0; i < 3; ++i) joiners.emplace_back([&] { if (Wait(Base(), &kThr, 1, false, kInfinite) == 0) ++joined; });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  Check(joined == 0, "three joiners wait for the thread");
  ThreadExited(Base(), kThr);
  for (auto& t : joiners) t.join();
  Check(joined == 3, "the thread ending releases every joiner");
  Check(Wait(Base(), &kThr, 1, false, 0) == 0 && Wait(Base(), &kThr, 1, false, 0) == 0, "an ended thread stays signalled (later waits return at once)");
  ThreadUnregister(kThr);
  Check(!IsNative(Base(), kThr), "an unregistered thread object is no longer ours");

  std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
