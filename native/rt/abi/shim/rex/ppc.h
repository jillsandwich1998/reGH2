// Shim of the SDK umbrella <rex/ppc.h> for the isolated CPU module (native/rt/abi).
// The SDK's version also pulls hook.h / function.h / stack.h, which depend on the kernel (KernelState, ThreadState, logging).
// The generated code needs only the CPU foundation below (PPCContext, PPCFunc, SIMD intrinsics, integer types).
#pragma once

#include <rex/types.h>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/ppc/intrinsics.h>
