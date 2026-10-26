// Hooks of the D3D library inside the game (G.0, option A). Observation only for now: each hook calls the original function and then tells
// the registered observer (gh2gfx/guest_d3d.h). With no observer registered a hook costs one atomic load.
//
// Guest side (shim headers only): no SDK header, no `rex::`.
#include "gh2_pch.h"  // PPCContext, REX_EXTERN

#include <gh2gfx/d3d_probe.h>
#include <gh2gfx/guest_d3d.h>

#include <atomic>

namespace gh2gfx::d3d {
namespace {
std::atomic<Observer*> g_observer{nullptr};
}
void SetObserver(Observer* observer) { g_observer.store(observer, std::memory_order_release); }
Observer* GetObserver() { return g_observer.load(std::memory_order_acquire); }
}  // namespace gh2gfx::d3d

namespace {

inline uint32_t LoadBE32(const uint8_t* base, uint32_t guest_address) {
  uint32_t v;
  __builtin_memcpy(&v, base + guest_address, sizeof(v));
  return __builtin_bswap32(v);
}

// Records a hand-hooked function in the discovery probe like the generated wrappers do (a no-op unless GH2_D3D_PROBE is set).
class ProbeScope {
 public:
  ProbeScope(uint32_t address, const PPCContext& ctx, const uint8_t* base) : ctx_(ctx), base_(base), on_(gh2gfx::d3d::ProbeEnabled()) {
    if (!on_) return;
    index_ = gh2gfx::d3d::ProbeIndexOf(address);
    args_[0] = ctx.r3.u32;
    args_[1] = ctx.r4.u32;
    args_[2] = ctx.r5.u32;
    args_[3] = ctx.r6.u32;
    args_[4] = ctx.r7.u32;
    args_[5] = ctx.r8.u32;
    if (index_ == 0xFFFFFFFFu) on_ = false;
    else gh2gfx::d3d::ProbeEnter(index_, args_, base);
  }
  ~ProbeScope() {
    if (on_) gh2gfx::d3d::ProbeLeave(index_, ctx_.r3.u32, args_, base_);
  }
  ProbeScope(const ProbeScope&) = delete;
  ProbeScope& operator=(const ProbeScope&) = delete;

 private:
  const PPCContext& ctx_;
  const uint8_t* base_;
  bool on_;
  uint32_t index_ = 0;
  uint32_t args_[6] = {};
};

}  // namespace

// ---- shader creation: the engine hands a container (a pointer into the loaded .fx_xbox data) to the library, which copies its virtual part
// into a new object and its microcode into GPU-visible memory. The container is intact here, which is what the shader cache hashes. ----
REX_EXTERN(__imp__sub_823CBE80);
REX_EXTERN(sub_823CBE80) {
  ProbeScope probe(0x823CBE80, ctx, base);
  const uint32_t container = ctx.r3.u32;
  __imp__sub_823CBE80(ctx, base);
  if (auto* o = gh2gfx::d3d::GetObserver()) o->OnShaderCreated(base, gh2gfx::d3d::Stage::kVertex, container, ctx.r3.u32);
}

REX_EXTERN(__imp__sub_823CBD70);
REX_EXTERN(sub_823CBD70) {
  ProbeScope probe(0x823CBD70, ctx, base);
  const uint32_t container = ctx.r3.u32;
  __imp__sub_823CBD70(ctx, base);
  if (auto* o = gh2gfx::d3d::GetObserver()) o->OnShaderCreated(base, gh2gfx::d3d::Stage::kPixel, container, ctx.r3.u32);
}

// ---- draw-state commit: called by the four draw functions right before they write DRAW_INDX. It patches the microcode of the shaders to the
// current vertex declaration / textures, writes IM_LOAD packets that point the GPU at it, and moves the command pointer. After it returns, the
// microcode the GPU will run for this draw sits in memory and the packets that bind it are in (cmd_before, cmd_after]. ----
REX_EXTERN(__imp__sub_823D4CC8);
REX_EXTERN(sub_823D4CC8) {
  const uint32_t device = ctx.r3.u32;
  if (gh2gfx::d3d::ProbeEnabled()) gh2gfx::d3d::ProbeSetDevice(device);  // the probe's window mode reads the command pointer around every call
  ProbeScope probe(0x823D4CC8, ctx, base);
  auto* o = gh2gfx::d3d::GetObserver();
  if (!o) {
    __imp__sub_823D4CC8(ctx, base);
    return;
  }
  const uint32_t before = LoadBE32(base, device + gh2gfx::d3d::layout::kDeviceCommandPointer);
  __imp__sub_823D4CC8(ctx, base);
  const uint32_t after = LoadBE32(base, device + gh2gfx::d3d::layout::kDeviceCommandPointer);
  o->OnDrawStateCommit(base, device, before, after);
}

// ---- the rest of the API a frame is described with (Phase 2.6). Every hook below calls the original and then reports the call (registers at
// entry, result, and for the functions that write PM4 the command pointer around it). See Api in gh2gfx/guest_d3d.h for what each function is. ----
namespace {

using gh2gfx::d3d::Api;
using gh2gfx::d3d::Call;

template <bool kTrackCommandPointer>
inline void Hooked(PPCContext& ctx, uint8_t* base, PPCFunc* original, uint32_t address, Api api) {
  gh2gfx::d3d::Observer* o = gh2gfx::d3d::GetObserver();
  ProbeScope probe(address, ctx, base);
  if (!o) {
    original(ctx, base);
    return;
  }
  Call c;
  c.api = api;
  c.address = address;
  c.r[0] = ctx.r3.u32;
  c.r[1] = ctx.r4.u32;
  c.r[2] = ctx.r5.u32;
  c.r[3] = ctx.r6.u32;
  c.r[4] = ctx.r7.u32;
  c.r[5] = ctx.r8.u32;
  c.r[6] = ctx.r9.u32;
  c.r[7] = ctx.r10.u32;
  c.f1 = ctx.f1.f64;
  if (kTrackCommandPointer) c.cmd_before = LoadBE32(base, c.r[0] + gh2gfx::d3d::layout::kDeviceCommandPointer);
  original(ctx, base);
  c.ret = ctx.r3.u32;
  if (kTrackCommandPointer) c.cmd_after = LoadBE32(base, c.r[0] + gh2gfx::d3d::layout::kDeviceCommandPointer);
  o->OnCall(base, c);
}

}  // namespace

#define GH2_D3D_API(ADDR, API, TRACK)                                                          \
  REX_EXTERN(__imp__sub_##ADDR);                                                                \
  REX_EXTERN(sub_##ADDR) { Hooked<TRACK>(ctx, base, __imp__sub_##ADDR, 0x##ADDR, Api::API); }

// vertex input
GH2_D3D_API(823CA620, kSetVertexDeclaration, false)
GH2_D3D_API(823CBF70, kSetFVF, false)
GH2_D3D_API(823CA638, kCreateVertexDeclaration, false)
GH2_D3D_API(823C3C68, kCreateVertexBuffer, false)
GH2_D3D_API(823C3D90, kCreateIndexBuffer, false)
GH2_D3D_API(823BFE20, kSetStreamSource, false)
GH2_D3D_API(823BFFC8, kSetIndices, false)
// textures and samplers
GH2_D3D_API(823BD238, kCreateTexture, false)
GH2_D3D_API(823BCBD8, kCreateTextureHeader, false)
GH2_D3D_API(823BD358, kCreateSurface, false)
GH2_D3D_API(823BD580, kSetTexture, false)
// the 20 functions that edit a field of a sampler's fetch constant (device, sampler, value)
GH2_D3D_API(823BF2F0, kSetSamplerState, false)
GH2_D3D_API(823BF3E0, kSetSamplerState, false)
GH2_D3D_API(823BF478, kSetSamplerState, false)
GH2_D3D_API(823BF568, kSetSamplerState, false)
GH2_D3D_API(823BF600, kSetSamplerState, false)
GH2_D3D_API(823BF658, kSetSamplerState, false)
GH2_D3D_API(823BF6F0, kSetSamplerState, false)
GH2_D3D_API(823BF768, kSetSamplerState, false)
GH2_D3D_API(823BF7C8, kSetSamplerState, false)
GH2_D3D_API(823BF810, kSetSamplerState, false)
GH2_D3D_API(823BF870, kSetSamplerState, false)
GH2_D3D_API(823BF8B0, kSetSamplerState, false)
GH2_D3D_API(823BF930, kSetSamplerState, false)
GH2_D3D_API(823BF9B0, kSetSamplerState, false)
GH2_D3D_API(823BFA20, kSetSamplerState, false)
GH2_D3D_API(823BFA70, kSetSamplerState, false)
GH2_D3D_API(823BFAC0, kSetSamplerState, false)
GH2_D3D_API(823BFB10, kSetSamplerState, false)
GH2_D3D_API(823BFB68, kSetSamplerState, false)
GH2_D3D_API(823BFBC0, kSetSamplerState, false)
// render targets and the frame
GH2_D3D_API(823C0718, kSetRenderTarget, false)
GH2_D3D_API(823C0A80, kSetDepthStencilSurface, false)
GH2_D3D_API(823C2770, kResolve, true)
GH2_D3D_API(823C1EA8, kClear, true)
GH2_D3D_API(823CF950, kSwap, true)
// draws (every geometry DRAW_INDX of the library is written by one of these four; BeginVertices/EndVertices bracket the inline vertex data)
GH2_D3D_API(823C7A08, kDrawVertices, true)
GH2_D3D_API(823C7DF0, kDrawIndexedVertices, true)
GH2_D3D_API(823C74D8, kDrawIndexedVerticesUP, true)
GH2_D3D_API(823C6FB8, kBeginVertices, true)
GH2_D3D_API(823C7458, kEndVertices, true)
// CPU access to GPU-visible memory
GH2_D3D_API(823C3D30, kLockVertexBuffer, false)
GH2_D3D_API(823C3E40, kLockIndexBuffer, false)
GH2_D3D_API(823BCA80, kTextureLevelAddress, false)

#undef GH2_D3D_API

