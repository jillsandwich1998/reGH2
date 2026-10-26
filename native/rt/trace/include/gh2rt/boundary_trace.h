// gh2rt/boundary_trace.h - tracer of the recomp <-> runtime boundary.
//
// Every kernel import the guest calls (plus, from the backend, MMIO accesses and indirect-call resolutions) is counted exactly
// and recorded as a 48-byte event in a binary file, so two runtimes (today: the ReXGlue-backed one and, later, ours) can be
// compared at the one place where they must agree. Enabled with GH2_BOUNDARY_TRACE=<file>; when unset the cost is one branch.
//
// Files written: <file> (events), <file>.meta.json (import table, parameters), <file>.counts.json (exact counts, rewritten every
// second so a SIGKILLed run still has them). Events: the first GH2_BOUNDARY_TRACE_FULL (default 2000) calls of each import are all
// recorded, afterwards one in GH2_BOUNDARY_TRACE_SAMPLE (default 128); the counts are always exact.
//
// This header must stay free of SDK headers and of the `rex::` namespace (native/tools/check_isolation.sh).
#pragma once

#include <cstdint>
#include <type_traits>

namespace gh2rt::trace {

inline constexpr uint16_t kUnknownId = 0xFFFF;
inline constexpr const char* kPseudoMmioLoad = "<mmio_load>";
inline constexpr const char* kPseudoMmioStore = "<mmio_store>";
inline constexpr const char* kPseudoResolveIndirect = "<resolve_indirect>";

namespace detail {
extern bool g_enabled;  // set once, before main(), from the environment
}
inline bool Enabled() { return detail::g_enabled; }

// Index of an import (or pseudo event) in the trace's name table, or kUnknownId.
uint16_t IdOf(const char* name);

// Records one event (thread-safe). `flags`: bit0 = recorded by a hand-written hook, bit1 = pseudo event.
void Record(uint16_t id, const uint32_t (&args)[6], uint32_t ret, uint16_t flags = 0);
// MMIO access / indirect resolution: a0 = guest address, a1 = value.
void RecordPseudo(uint16_t id, uint32_t a0, uint32_t a1, uint32_t ret);

// Captures r3..r8 on entry and r3 on exit of one import call. `Ctx` is PPCContext (kept a template so this header does not
// need the context definition).
template <class Ctx>
class Scope {
 public:
  Scope(uint16_t id, Ctx& ctx, uint16_t flags = 0) : ctx_(ctx), id_(id), flags_(flags), on_(Enabled()) {
    if (on_) {
      args_[0] = ctx.r3.u32;
      args_[1] = ctx.r4.u32;
      args_[2] = ctx.r5.u32;
      args_[3] = ctx.r6.u32;
      args_[4] = ctx.r7.u32;
      args_[5] = ctx.r8.u32;
    }
  }
  ~Scope() {
    if (on_) Record(id_, args_, ctx_.r3.u32, flags_);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Ctx& ctx_;
  uint32_t args_[6];
  uint16_t id_;
  uint16_t flags_;
  bool on_;
};

}  // namespace gh2rt::trace

// First statement of a hand-written import hook (REX_EXTERN(__wrap___imp__X) { GH2_TRACE_HOOK("X"); ... }): traces the call
// whichever way the hook returns.
#define GH2_TRACE_HOOK(NAME)                                                                          \
  static const uint16_t gh2_trace_id_ = ::gh2rt::trace::IdOf(NAME);                                   \
  ::gh2rt::trace::Scope<std::remove_cvref_t<decltype(ctx)>> gh2_trace_scope_(gh2_trace_id_, ctx, 1)
