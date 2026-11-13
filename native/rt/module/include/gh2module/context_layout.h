// gh2module/context_layout.h - a fingerprint of the PPCContext layout, compiled by both sides of the game module boundary.
//
// The host (MSVC ABI) and the game module (llvm-mingw, Itanium ABI) share PPCContext by reference. It is a plain struct (no virtuals, no
// bit-fields), so both ABIs lay it out the same; this fingerprint proves it at load time instead of trusting it: size, alignment and the
// offset of every group of registers, folded into one number that the handshake compares.
// Include after the shim's <rex/ppc/context.h>.
#pragma once

#include <cstddef>
#include <cstdint>

#include <rex/ppc/context.h>

namespace gh2module {

constexpr uint64_t LayoutMix(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }

constexpr uint64_t ContextLayout() {
  uint64_t h = 14695981039346656037ull;
  for (uint64_t v : {uint64_t(sizeof(PPCContext)), uint64_t(alignof(PPCContext)), uint64_t(sizeof(PPCRegister)), uint64_t(sizeof(PPCVRegister)),
                     uint64_t(offsetof(PPCContext, r3)), uint64_t(offsetof(PPCContext, r0)), uint64_t(offsetof(PPCContext, r1)),
                     uint64_t(offsetof(PPCContext, r13)), uint64_t(offsetof(PPCContext, r14)), uint64_t(offsetof(PPCContext, r31)),
                     uint64_t(offsetof(PPCContext, lr)), uint64_t(offsetof(PPCContext, ctr)), uint64_t(offsetof(PPCContext, xer)),
                     uint64_t(offsetof(PPCContext, reserved)), uint64_t(offsetof(PPCContext, msr)), uint64_t(offsetof(PPCContext, cr0)),
                     uint64_t(offsetof(PPCContext, cr7)), uint64_t(offsetof(PPCContext, fpscr)), uint64_t(offsetof(PPCContext, vscr_sat)),
                     uint64_t(offsetof(PPCContext, last_indirect_target)), uint64_t(offsetof(PPCContext, f0)), uint64_t(offsetof(PPCContext, f31)),
                     uint64_t(offsetof(PPCContext, v0)), uint64_t(offsetof(PPCContext, v127))})
    h = LayoutMix(h, v);
  return h;
}

}  // namespace gh2module
