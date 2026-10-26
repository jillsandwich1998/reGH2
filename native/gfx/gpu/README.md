# gh2gpu - the GPU's command processor, natively (Phase 3)

Replaces the runtime's GraphicsSystem + CommandProcessor for the game (`--gpu=native`). Plain C++17, no SDK. The kernel imports `VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`,
`VdSetGraphicsInterruptCallback` and `VdSwap` land in `rt/hooks/hooks_gpu.cpp`; the MMIO window 0x7FC80000 (CP_RB_WPTR and friends) is routed here by the backend (`SetNativeMmio`).

## Protocol (FACT: read from the running game, checked by `gh2_gpu_selftest` and by the game running at 60.0 fps with the SDK's graphics system absent)
- The primary ring is a PM4 stream in guest PHYSICAL memory (host base = arena base + 0x100000000); the write pointer arrives through the MMIO window, the read pointer is written back (in dwords)
  to the write-back address every `block size` dwords. `WAIT_REG_MEM` polls, `EVENT_WRITE_SHD` writes the fence/swap counter, `INTERRUPT` and the vertical blank (60 Hz clock thread) call the game's
  callback on a guest thread (critical region held, TLS pointer zeroed, 176-byte padded stack, LR 0xBCBCBCBC).
- State: a register file of 0x5003 dwords; `SET_CONSTANT` (types: ALU float, fetch, bool, loop, plain), `LOAD_ALU_CONSTANT` (from memory), `IM_LOAD(_IMMEDIATE)` (shader microcode address),
  `DRAW_INDX(_2)`, `XE_SWAP` (signature 0x53574150), predicated tiling (`SET_BIN_MASK/SELECT`). `RB_MODECONTROL` edram mode 4 = colour+depth, 5 = depth only, 6 = copy (resolve).
- Everything the packets mean beyond this is the renderer's business (`../render`).

## Threading
`Run()` is one thread (a guest thread so that the interrupt callback can run guest code); the Mmio/UpdateWritePointer/Initialize calls come from any guest thread. The Sink is called from the
processor's thread, in stream order.

## Not done
No viz queries beyond reporting "finished", no tessellation, no memory-export (`MEMEXPORT`) shaders.
