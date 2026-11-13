// gh2k/host_addr.h - where a guest address lives in the host's address space (header only, no dependencies).
//
// The translated code reaches the guest's memory as `base + address`, except for the console's 4 KB-page physical window (0xE0000000..): guest 0xE0000000 + q is physical q + 0x1000.
// Linux maps that window exactly (a view at file offset 0x100001000), so the byte sits at base + address. Windows maps views only at 64 KB-aligned file offsets, so the window starts
// at physical 0 and the byte of guest address `a` sits 4 KB further, at base + a + 0x1000: the same rule the SDK and the translated code use (REX_PHYS_HOST_OFFSET in the generated
// gh2_pch.h, memory_win.cpp). Everything of ours that follows a pointer the guest handed over (a critical section, an event, a read buffer) must go through HostAddr.
#pragma once

#include <cstdint>

namespace gh2k {

#ifdef _WIN32
inline constexpr uint32_t kEWindowHostShift = 0x1000;
#else
inline constexpr uint32_t kEWindowHostShift = 0;
#endif

inline constexpr uint32_t HostOffset(uint32_t guest) { return guest >= 0xE0000000u ? kEWindowHostShift : 0u; }
inline uint8_t* HostAddr(uint8_t* base, uint32_t guest) { return base + guest + HostOffset(guest); }
inline const uint8_t* HostAddr(const uint8_t* base, uint32_t guest) { return base + guest + HostOffset(guest); }

}  // namespace gh2k
