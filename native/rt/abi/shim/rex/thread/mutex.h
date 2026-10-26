// Shim of <rex/thread/mutex.h>: only the global critical region is part of the contract (gh2abi::global_critical_region).
#pragma once

#include <gh2abi/abi.h>

namespace rex::thread {
using global_critical_region = ::gh2abi::global_critical_region;
}  // namespace rex::thread
