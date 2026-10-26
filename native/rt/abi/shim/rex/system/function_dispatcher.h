// Shim of <rex/system/function_dispatcher.h>: generated gh2_RegisterFunctions() takes an IModuleRegistrar (gh2abi).
#pragma once

#include <gh2abi/abi.h>

namespace rex::runtime {
using IModuleRegistrar = ::gh2abi::IModuleRegistrar;
}  // namespace rex::runtime
