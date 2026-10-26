// gh2gpu/registers.h - the Xenos GPU registers and PM4 packet constants the command processor and the renderer use (Phase 3).
//
// Register numbers are dword indices into the 0x5003-register file (the byte offset in the MMIO window 0x7FC80000 is index * 4). They are facts of the hardware as the game's Direct3D
// library uses it, taken from the registers it writes (checked against the command buffers of the running game, see gfx/guest_d3d/README.md and gfx/gpu/README.md), not code.
// Plain C++17, no SDK.
#pragma once

#include <cstdint>

namespace gh2gpu {

inline constexpr uint32_t kRegisterCount = 0x5003;
inline constexpr uint32_t kMmioBase = 0x7FC80000u;   // the window the guest sees; registers are (address & 0xFFFF) / 4
inline constexpr uint32_t kMmioMask = 0xFFFF0000u;

namespace reg {
// command processor / system
inline constexpr uint32_t CP_RB_WPTR = 0x01C5;
inline constexpr uint32_t SCRATCH_UMSK = 0x01DC;
inline constexpr uint32_t SCRATCH_ADDR = 0x01DD;
inline constexpr uint32_t SCRATCH_REG0 = 0x0578;   // ..SCRATCH_REG0 + 7
inline constexpr uint32_t WAIT_UNTIL = 0x05C8;
inline constexpr uint32_t COHER_BASE_HOST = 0x0A30;
inline constexpr uint32_t COHER_SIZE_HOST = 0x0A2F;
inline constexpr uint32_t COHER_STATUS_HOST = 0x0A31;
inline constexpr uint32_t PA_SC_VIZ_QUERY_STATUS_0 = 0x0C44;
inline constexpr uint32_t PA_SC_VIZ_QUERY_STATUS_1 = 0x0C45;
inline constexpr uint32_t RB_EDRAM_TIMING = 0x0F00;
inline constexpr uint32_t RB_BC_CONTROL = 0x0F01;
// display controller (the D1 CRTC the game polls for the vertical blank)
inline constexpr uint32_t D1MODE_V_COUNTER = 0x194C;
inline constexpr uint32_t D1MODE_VBLANK_VLINE_STATUS = 0x1951;
inline constexpr uint32_t D1MODE_VIEWPORT_SIZE = 0x1961;
// vertex group / draw
inline constexpr uint32_t VGT_MAX_VTX_INDX = 0x2100;
inline constexpr uint32_t VGT_MIN_VTX_INDX = 0x2101;
inline constexpr uint32_t VGT_INDX_OFFSET = 0x2102;
inline constexpr uint32_t VGT_EVENT_INITIATOR = 0x21F9;
inline constexpr uint32_t VGT_DMA_BASE = 0x21FA;
inline constexpr uint32_t VGT_DMA_SIZE = 0x21FB;
inline constexpr uint32_t VGT_DRAW_INITIATOR = 0x21FC;
// render backend
inline constexpr uint32_t RB_MODECONTROL = 0x2208;
inline constexpr uint32_t PA_SC_VIZ_QUERY = 0x2293;
inline constexpr uint32_t RB_COPY_CONTROL = 0x2318;
inline constexpr uint32_t RB_COPY_DEST_BASE = 0x2319;
inline constexpr uint32_t RB_COPY_DEST_PITCH = 0x231A;
inline constexpr uint32_t RB_COPY_DEST_INFO = 0x231B;
inline constexpr uint32_t RB_SAMPLE_COUNT_ADDR = 0x2325;
// shader constant files (SET_CONSTANT / LOAD_ALU_CONSTANT write here)
inline constexpr uint32_t SHADER_CONSTANT_000_X = 0x4000;       // 512 float4 vertex + pixel constants: 2048 dwords
inline constexpr uint32_t SHADER_CONSTANT_FETCH_00_0 = 0x4800;  // 32 fetch constants x 6 dwords
inline constexpr uint32_t SHADER_CONSTANT_BOOL_000_031 = 0x4900;
inline constexpr uint32_t SHADER_CONSTANT_LOOP_00 = 0x4908;
}  // namespace reg

namespace pm4 {
// type 3 opcodes
inline constexpr uint32_t NOP = 0x10;
inline constexpr uint32_t REG_RMW = 0x21;
inline constexpr uint32_t DRAW_INDX = 0x22;
inline constexpr uint32_t VIZ_QUERY = 0x23;
inline constexpr uint32_t WAIT_FOR_IDLE = 0x26;
inline constexpr uint32_t IM_LOAD = 0x27;
inline constexpr uint32_t IM_LOAD_IMMEDIATE = 0x2B;
inline constexpr uint32_t SET_CONSTANT = 0x2D;
inline constexpr uint32_t LOAD_ALU_CONSTANT = 0x2F;
inline constexpr uint32_t DRAW_INDX_2 = 0x36;
inline constexpr uint32_t INDIRECT_BUFFER_PFD = 0x37;
inline constexpr uint32_t INVALIDATE_STATE = 0x3B;
inline constexpr uint32_t WAIT_REG_MEM = 0x3C;
inline constexpr uint32_t MEM_WRITE = 0x3D;
inline constexpr uint32_t REG_TO_MEM = 0x3E;
inline constexpr uint32_t INDIRECT_BUFFER = 0x3F;
inline constexpr uint32_t COND_WRITE = 0x45;
inline constexpr uint32_t EVENT_WRITE = 0x46;
inline constexpr uint32_t ME_INIT = 0x48;
inline constexpr uint32_t SET_BIN_MASK = 0x50;
inline constexpr uint32_t SET_BIN_SELECT = 0x51;
inline constexpr uint32_t INTERRUPT = 0x54;
inline constexpr uint32_t SET_CONSTANT2 = 0x55;
inline constexpr uint32_t SET_SHADER_CONSTANTS = 0x56;
inline constexpr uint32_t EVENT_WRITE_SHD = 0x58;
inline constexpr uint32_t EVENT_WRITE_EXT = 0x5A;
inline constexpr uint32_t EVENT_WRITE_ZPD = 0x5B;
inline constexpr uint32_t CONTEXT_UPDATE = 0x5E;
inline constexpr uint32_t SET_BIN_MASK_LO = 0x60;
inline constexpr uint32_t SET_BIN_MASK_HI = 0x61;
inline constexpr uint32_t SET_BIN_SELECT_LO = 0x62;
inline constexpr uint32_t SET_BIN_SELECT_HI = 0x63;
inline constexpr uint32_t XE_SWAP = 0x64;   // not hardware: VdSwap writes it so the processor knows a frame is complete
inline constexpr uint32_t kSwapSignature = 0x53574150u;   // 'SWAP'
}  // namespace pm4

enum class Endian : uint32_t { kNone = 0, k8in16 = 1, k8in32 = 2, k16in32 = 3 };

inline uint32_t GpuSwap(uint32_t value, Endian endian) {
  switch (endian) {
    case Endian::k8in16: return ((value << 8) & 0xFF00FF00u) | ((value >> 8) & 0x00FF00FFu);
    case Endian::k8in32: return __builtin_bswap32(value);
    case Endian::k16in32: return (value >> 16) | (value << 16);
    default: return value;
  }
}

// VGT_DRAW_INITIATOR
inline uint32_t DrawPrimType(uint32_t v) { return v & 0x3F; }
inline uint32_t DrawSourceSelect(uint32_t v) { return (v >> 6) & 0x3; }   // 0 DMA, 1 immediate, 2 auto index
inline uint32_t DrawMajorMode(uint32_t v) { return (v >> 8) & 0x3; }
inline bool DrawIndex32(uint32_t v) { return (v >> 11) & 1; }
inline uint32_t DrawNumIndices(uint32_t v) { return v >> 16; }

}  // namespace gh2gpu
