// Shim of <rex/chrono/clock.h>: only the guest timebase is part of the contract (gh2abi::Clock).
#pragma once

#include <gh2abi/abi.h>

namespace rex::chrono {
using Clock = ::gh2abi::Clock;
}  // namespace rex::chrono
