// Shim of <rex/system/mmio_handler.h>: the MMIO window is part of the contract (gh2abi::MMIOHandler).
#pragma once

#include <gh2abi/abi.h>

namespace rex::runtime {
using MMIOHandler = ::gh2abi::MMIOHandler;
}  // namespace rex::runtime
