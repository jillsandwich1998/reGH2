// gh2abi/abi.h - the closed contract between the recompiled game code (and the hooks) and the runtime.
//
// Everything the generated C++ may call that is NOT generated code itself is declared here, in namespace gh2abi, plus the
// 179 kernel imports (`extern "C" void __imp__<Name>(PPCContext&, uint8_t*)`, listed in contract/imports.txt) and the
// post-codegen hook gh2_spin_hint(). The generated text spells these names `rex::...`; the headers under include/rex/ are
// one-line aliases onto this namespace so the generated files compile unchanged (see docs/abi_recomp_runtime.md).
//
// A backend implements this header. Today: native/rt/backend_rex (forwards to the ReXGlue SDK). Later: our own runtime.
// This header must stay free of SDK headers and of the `rex::` namespace.
#pragma once

#include <cstdint>
#include <mutex>
#include <string_view>

struct PPCContext;
using PPCFunc = void(PPCContext& ctx, uint8_t* base);

namespace gh2abi {

// 1) Indirect call fallback (REX_CALL_INDIRECT_FUNC): the target is outside the module's dispatch table or its slot is
//    still empty. Returns the host function for a guest address (never null for valid code).
PPCFunc* ResolveIndirectFunction(uint32_t guest_address);

// 2) Guest timebase (`mftb`), in guest ticks.
struct Clock {
  static uint64_t QueryGuestTickCount();
};

// 3) Global critical region: the guest's "interrupts disabled" state (`mtmsrd`/`mfmsr`) maps onto this recursive mutex,
//    which kernel code shares with the guest.
struct global_critical_region {
  static std::recursive_mutex& mutex();
  static std::unique_lock<std::recursive_mutex> AcquireDirect() {
    return std::unique_lock<std::recursive_mutex>(mutex());
  }
};

// 4) MMIO window 0x7F000000..0x7FFFFFFF (GPU registers 0x7FC8xxxx, XMA registers 0x7FEAxxxx). Only the generated REX_MM_*
//    macros reach it. CheckLoad/CheckStore return true when a device handled the access.
class MMIOHandler {
 public:
  static MMIOHandler* global_handler();
  bool CheckLoad(uint32_t guest_address, uint32_t* out_value);
  bool CheckStore(uint32_t guest_address, uint32_t value);
};

// 5) Diagnostics emitted from generated inline code (service traps, `tw` traps).
enum class LogLevel : int { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };
void LogMessage(LogLevel level, std::string_view message);

// 6) Function registration for multi-module projects (generated gh2_RegisterFunctions()).
class IModuleRegistrar {
 public:
  virtual bool SetFunction(uint32_t guest_address, PPCFunc* func) = 0;

 protected:
  ~IModuleRegistrar() = default;
};

// 8) Interim, until the kernel is ours: kernel code of the runtime asks "which XThread am I?" and has no answer on a thread that gh2k::thread created. NoteRuntimeThread() is
//    called on a thread of the runtime (the creator, e.g. the main thread) and remembers it; AdoptCurrentHostThread() is called first thing on a native thread and lets the
//    imports that are still forwarded to the runtime treat it as that thread (identity only: its stack, TLS, KTHREAD and PCR stay the native ones).
void NoteRuntimeThread();
// Called first thing on a native thread with its ids: binds the thread to the runtime (the "current XThread" and the "current ThreadState" that kernel code asks for) and returns the
// PPCContext the thread must run guest code with, already holding r1 = stack_top and r13 = pcr; null if the backend has no such notion (the thread then uses a context of its own).
PPCContext* AdoptCurrentHostThread(uint32_t thread_id, uint32_t stack_top, uint32_t pcr);
void ReleaseCurrentHostThread();   // last thing on the thread (also on the way out of pthread_exit)

// 9) MMIO windows the hooks serve themselves (the GPU registers, 0x7FC80000): the backend asks this handler first and falls back to the runtime's for everything it does not handle.
struct NativeMmio {
  bool (*load)(void* user, uint32_t address, uint32_t* value) = nullptr;    // true: handled
  bool (*store)(void* user, uint32_t address, uint32_t value) = nullptr;
  void* user = nullptr;
};
void SetNativeMmio(const NativeMmio& handler);

}  // namespace gh2abi

// 7) Post-codegen replacement of the guest SMT-yield hint `db16cyc` (see scripts/gh2install/pipeline.py). Defined by
//    native/rt/hooks.
extern "C" void gh2_spin_hint();
