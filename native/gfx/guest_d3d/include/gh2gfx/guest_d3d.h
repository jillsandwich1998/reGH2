// gh2gfx/guest_d3d.h - the D3D library that is linked into the game, seen from the host (Phase 2, G.0, option A).
//
// The game does not talk to the GPU through the kernel: the XDK's D3D library is compiled into the XEX (0x823BB8C0-0x823DD2D0: the device
// methods at 0x823BB8C0-0x823C0000, the core at 0x823C0000-0x823E0000; 909 functions between 0x823B0000 and 0x823E0000 are probed) and writes
// PM4 packets itself. Option A hooks that library's entry points instead of processing the packets. This
// header is the contract between the hooks (native/gfx/guest_d3d/d3d_hooks.cpp, guest side, shim headers only) and whoever listens:
// today an observer that correlates what the library binds with the shader cache; later the renderer.
//
// The library has no symbols: every address and offset below was found by reading its recompiled code and CONFIRMED against the running
// game with the discovery probe (gh2gfx/d3d_probe.h); native/gfx/guest_d3d/README.md says how each was verified.
// No SDK header, no `rex::` (native/tools/check_isolation.sh).
#pragma once

#include <cstdint>

namespace gh2gfx::d3d {

enum class Stage : uint8_t { kVertex = 0, kPixel = 1 };

// ---- the rest of the API the game uses to describe a frame (Phase 2.6). Names are the XDK's; the library has none, so each one is the
// reading of its code plus the arguments seen at run time (README.md has the evidence). ----
enum class Api : uint8_t {
  // vertex input
  kSetVertexDeclaration,    // 0x823CA620 (device, declaration)           stores device+11684 and sets a dirty bit
  kSetFVF,                  // 0x823CBF70 (device, fvf)                   converts to a declaration, stores device+12296 and device+11684
  kCreateVertexDeclaration, // 0x823CA638 (D3DVERTEXELEMENT[] , ...)      -> declaration object (type 5)
  kCreateVertexBuffer,      // 0x823C3C68 (length, usage, ...)            -> vertex buffer object (type 1); no device argument
  kCreateIndexBuffer,       // 0x823C3D90 (length, usage, format)         -> index buffer object (type 2)
  kSetStreamSource,         // 0x823BFE20 (device, stream, vb, offset, stride, dirty bit)   stream n = vertex fetch constant 95 - n
  kSetIndices,              // 0x823BFFC8 (device, ib)                    stores device+12300
  // textures and samplers
  kCreateTexture,           // 0x823BD238 (width, height, levels, ..., format)  -> texture header (type 3), no device argument
  kCreateTextureHeader,     // 0x823BCBD8 (type, width, height, ...)      header initialisation (XGSetTextureHeader); the game calls it directly too
  kCreateSurface,           // 0x823BD358 (width, height, format, multisample, ...) -> surface object (type 4): render target or depth stencil
  kSetTexture,              // 0x823BD580 (device, sampler, texture, dirty bit)  copies the header's fetch constant into the device's shadow
  kSetSamplerState,         // 20 functions 0x823BF2F0-0x823BFBC0 (device, sampler, value): each one edits a bit field of the sampler's fetch constant
  // render targets and the frame
  kSetRenderTarget,         // 0x823C0718 (device, index, surface)        (0x823C0D58 jumps to it)
  kSetDepthStencilSurface,  // 0x823C0A80 (device, surface)
  kResolve,                 // 0x823C2770 (device, flags, source rect*, destination texture*, destination point*, level, ...)
  kClear,                   // 0x823C1EA8 (device, flags, rect*, color*, stencil..., f1 = depth)   one rectangle; Clear (0x823C1FD0) loops over it
  kSwap,                    // 0x823CF950 (device, front buffer texture*, ...)   calls the kernel's VdSwap
  // draws: exactly the four functions that write DRAW_INDX with geometry (the RECTLIST draws belong to Clear and Resolve)
  kDrawVertices,            // 0x823C7A08 (device, primitive, start vertex, vertex count)       vertices from the streams, indices generated
  kDrawIndexedVertices,     // 0x823C7DF0 (device, primitive, -, start index, index count)     indices from the bound index buffer (DMA)
  kDrawIndexedVerticesUP,   // 0x823C74D8 (device, primitive, ..., index count, ..., user indices and vertices)  both inline in the command buffer
  kBeginVertices,           // 0x823C6FB8 (device, primitive, vertex count, stride) -> pointer to the space for inline vertices; DrawVerticesUP is this + memcpy + End
  kEndVertices,             // 0x823C7458 (device)
  // CPU access to GPU-visible memory (the places where a loader or a dynamic buffer writes data)
  kLockVertexBuffer,        // 0x823C3D30 (vb, ...)
  kLockIndexBuffer,         // 0x823C3E40 (ib, ...)
  kTextureLevelAddress,     // 0x823BCA80 (texture, level, face, ...) -> where that mip level lives
  kCount
};
const char* ApiName(Api api);

// What a hook reports: the registers at entry, the result, and where the library's command pointer was before and after (draws, clears,
// resolves and swaps only; 0 otherwise).
struct Call {
  Api api = Api::kCount;
  uint32_t address = 0;          // the guest function (identifies the state for kSetSamplerState)
  uint32_t r[8] = {};            // r3..r10 at entry
  double f1 = 0;                 // f1 at entry (clear depth)
  uint32_t ret = 0;              // r3 at exit
  uint32_t cmd_before = 0, cmd_after = 0;  // device command pointer ("last written dword") around the call
  uint32_t device() const { return r[0]; }
};

// ---- guest functions (all take their arguments in r3..; the original is always called) ----
inline constexpr uint32_t kCreateVertexShader = 0x823CBE80;  // r3 = shader container (0x102A1101...) -> r3 = shader object (0 on failure)
inline constexpr uint32_t kCreatePixelShader = 0x823CBD70;   // r3 = shader container (0x102A1100...) -> r3 = shader object
inline constexpr uint32_t kCommitDrawState = 0x823D4CC8;     // r3 = device, r4 = dirty flags: binds shaders / state before a draw (DRAW_INDX comes next)

// ---- structure layouts ----
namespace layout {
// D3DDevice
inline constexpr uint32_t kDevicePixelShader = 12556;    // the pixel shader object bound now (0 = none)
inline constexpr uint32_t kDeviceVertexShader = 12560;   // the vertex shader object bound now
inline constexpr uint32_t kDeviceCommandPointer = 48;    // address of the LAST dword written to the command buffer (pre-increment convention)
// shader objects: the creation functions copy the container's "virtual" part into the object and its "physical" part (the microcode) into a
// separate GPU-visible allocation whose address is stored in the object
inline constexpr uint32_t kPixelUcodePointer = 24;
inline constexpr uint32_t kVertexUcodePointer = 32;
inline constexpr uint32_t kPixelContainerCopy = 40;      // header (0x102A11xx, virtualSize, physicalSize, ..., shader offset at +24) starts here
inline constexpr uint32_t kVertexContainerCopy = 872;    // vertex objects have 832 more bytes of per-declaration fetch state in front

// D3DDevice, state that the draw functions read (Phase 2.6; every offset is read from the code of the setters and of sub_823C7DF0, and cross-checked
// at run time by the observer's verify mode)
inline constexpr uint32_t kDeviceFetchConstants = 1024;  // 32 slots x 24 bytes = 6 dwords each: the shadow of the GPU registers 0x4800.. (texture fetch constants;
                                                         // a slot holds three 2-dword vertex fetch constants when it serves vertex buffers)
inline constexpr uint32_t kFetchSlotBytes = 24;
inline constexpr uint32_t kDeviceVertexDeclaration = 11684;  // the declaration object bound now
inline constexpr uint32_t kDeviceFvf = 12296;
inline constexpr uint32_t kDeviceIndexBuffer = 12300;
inline constexpr uint32_t kDeviceRenderTarget = 12304;   // + 4 * index (4 render targets)
inline constexpr uint32_t kDeviceDepthStencil = 12320;
inline constexpr uint32_t kDeviceStreamSource = 12324;   // + 4 * stream (16 streams): the vertex buffer object
inline constexpr uint32_t kDeviceStreamStride = 12392;   // + stream, one byte each: the stride in dwords
inline constexpr uint32_t kDeviceTexture = 12408;        // + 4 * sampler (26 samplers): the texture object
inline constexpr uint32_t kStreamCount = 16, kSamplerCount = 26, kRenderTargetCount = 4;
// Vertex stream n lives in vertex fetch constant 95 - n; constant j is dwords 2*(j%3) and 2*(j%3)+1 of fetch slot j/3.
inline constexpr uint32_t kVertexFetchConstantOfStream0 = 95;

// Resource objects (D3DResource): a 24-byte header {Common, ReferenceCount, Fence, ReadFence, Identifier, BaseFlush} and then the type's own fields.
inline constexpr uint32_t kResourceCommon = 0;           // low nibble = the resource type (ResourceType below); 0x00100000 = created by D3D
inline constexpr uint32_t kBufferAddress = 24;           // vertex buffer: the vertex fetch constant {address | 3, size in bytes | endian | 0x10000000}
inline constexpr uint32_t kBufferSize = 28;              // index buffer: address at +24 and size in bytes at +28
inline constexpr uint32_t kTextureFetchConstant = 28;    // texture: 6 dwords (GPUTEXTURE_FETCH_CONSTANT) at +28..+48; +24 is the mip flush word
inline constexpr uint32_t kSurfaceInfo = 24;             // surface: RB_SURFACE_INFO (pitch in bits 13:0), RB_COLOR_INFO / RB_DEPTH_INFO at +28, D3DFORMAT at +44
inline constexpr uint32_t kSurfaceFormat = 44;
}  // namespace layout

// D3DRESOURCETYPE as it appears in the low nibble of a resource's Common word
enum class ResourceType : uint8_t {
  kVertexBuffer = 1, kIndexBuffer = 2, kTexture = 3, kSurface = 4, kVertexDeclaration = 5, kVertexShader = 6, kPixelShader = 7,
};

// ---- container header (big endian, offsets from the container start) ----
namespace container {
inline constexpr uint32_t kFlags = 0;          // 0x102A11xx; bit 0 = 1 vertex, 0 pixel
inline constexpr uint32_t kVirtualSize = 4;
inline constexpr uint32_t kPhysicalSize = 8;
inline constexpr uint32_t kShaderOffset = 24;  // offset of the shader struct: u32 physical offset of the microcode, u32 its size in bytes
}  // namespace container

// The library's virtual -> physical conversion for the addresses it writes to the GPU (read from sub_823D4CC8, sub_823BD580 and sub_823BFE20): bits
// 28..0 of the address, plus 0x1000 for the 4 KB-page view (addresses from 0xE0000000 up). Low bits (fetch constant type, endian) pass through.
inline constexpr uint32_t PhysicalAddress(uint32_t virt) { return (virt & 0x1FFFFFFFu) + (((virt >> 20) + 0x200u) & 0x1000u); }

// What the hooks report. `base` translates guest addresses (host pointer = base + guest address); every guest value is big endian in memory.
class Observer {
 public:
  virtual ~Observer() = default;
  // A shader object was created from the container at `container` (a pointer into the engine's loaded .fx_xbox data). `object` is 0 on failure.
  virtual void OnShaderCreated(const uint8_t* base, Stage stage, uint32_t container, uint32_t object) = 0;
  // The library finished binding the state of one draw. The dwords it appended to the command buffer are the ones after `cmd_before`
  // up to and including `cmd_after` (both are "last written dword" addresses); equal when nothing was written.
  virtual void OnDrawStateCommit(const uint8_t* base, uint32_t device, uint32_t cmd_before, uint32_t cmd_after) = 0;
  // One of the hooked API functions returned (the list is Api above). Called on the guest thread that made the call, after the original ran.
  virtual void OnCall(const uint8_t* base, const Call& call) { (void)base; (void)call; }
};

// One observer at a time; none by default, in which case the hooks only call the original functions.
void SetObserver(Observer* observer);
Observer* GetObserver();

}  // namespace gh2gfx::d3d
