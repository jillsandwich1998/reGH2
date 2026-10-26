// Assertions for the isolated CPU module (native/rt/abi). Written for this project (not the SDK's header of the same name, which
// derives from the Xenia project): the generated code and the vendored CPU headers use no assertion macro of their own, so the
// standard <cassert> is all that is offered. rex/types.h includes this header; keep it free of any other dependency.
#pragma once

#include <cassert>
