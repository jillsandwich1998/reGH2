// gh2k/sync.h - the kernel's cheap synchronisation primitives, natively (Phase 2.7, K.2): IRQL, spin locks, critical sections, thread-local slots.
//
// Together they are 2.6 million of the 2.9 million kernel calls of a song (RtlEnter/LeaveCriticalSection 1.25 M each, the IRQL / spin lock group
// 2.5 M, KeTlsGetValue 154 k). They work on the structures the guest sees, in guest memory, in the layout the Xbox 360 kernel uses:
//   KPCR (r13):   +0x018 current IRQL (u8)   +0x100 current KTHREAD (be32, KPRCB.current_thread)
//   KTHREAD:      +0x068 address of the thread's dynamic TLS slots (be32)
//   KSPINLOCK:    be32, 0 = free, otherwise the PCR address of the owner
//   RTL_CRITICAL_SECTION: DISPATCHER_HEADER (0x10 bytes, +1 = spin count / 256), +0x10 lock count (raw int32, -1 = free, updated atomically),
//                         +0x14 recursion count (be32), +0x18 owning KTHREAD (be32)
// The thread that owns a critical section is identified by its KTHREAD, which the runtime that creates the guest threads fills in; nothing here creates a thread.
// Contended critical sections park the thread in a table keyed by the section's address (a wake token per Leave, so a wake-up before the sleep is not lost).
// Plain C++17, no SDK, no guest macros: `base` + guest address = host address.
#pragma once

#include <cstdint>

namespace gh2k::sync {

inline constexpr uint32_t kIrqlPassive = 0, kIrqlApc = 1, kIrqlDispatch = 2;
inline constexpr uint32_t kPcrCurrentIrql = 0x18, kPcrCurrentThread = 0x100, kThreadTlsSlots = 0x68;

// ---- IRQL and spin locks (all take the PCR = the guest's r13) ----
uint32_t RaiseIrql(uint8_t* base, uint32_t pcr, uint32_t new_irql);   // returns the old level
void LowerIrql(uint8_t* base, uint32_t pcr, uint32_t old_irql);
void SpinAcquire(uint8_t* base, uint32_t pcr, uint32_t lock);        // at the current IRQL
bool SpinTryAcquire(uint8_t* base, uint32_t pcr, uint32_t lock);
void SpinRelease(uint8_t* base, uint32_t lock);
uint32_t SpinAcquireRaising(uint8_t* base, uint32_t pcr, uint32_t lock);                    // KfAcquireSpinLock: raise to DISPATCH, take it, return the old IRQL
void SpinReleaseLowering(uint8_t* base, uint32_t pcr, uint32_t lock, uint32_t old_irql);    // KfReleaseSpinLock

// ---- critical sections ----
void CritInit(uint8_t* base, uint32_t cs, uint32_t spin_count);
void CritEnter(uint8_t* base, uint32_t pcr, uint32_t cs);
bool CritTryEnter(uint8_t* base, uint32_t pcr, uint32_t cs);
void CritLeave(uint8_t* base, uint32_t pcr, uint32_t cs);

// ---- thread-local slots ----
uint32_t TlsGet(uint8_t* base, uint32_t pcr, uint32_t index);
void TlsSet(uint8_t* base, uint32_t pcr, uint32_t index, uint32_t value);

struct Counters {
  uint64_t contended_enters;   // critical section enters that had to sleep
  uint64_t spin_waits;         // spin lock acquisitions that had to retry
};
Counters Stats();

}  // namespace gh2k::sync
