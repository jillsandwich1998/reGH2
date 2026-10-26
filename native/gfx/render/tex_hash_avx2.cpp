// XXH3 compiled for AVX2 (this file alone gets -mavx2: see CMakeLists.txt). Never call it on a processor without AVX2: tex_hash.cpp checks.
#define XXH_INLINE_ALL
#include <xxhash.h>

#include "tex_hash.h"

namespace gh2render {

uint64_t HashBytesAvx2(const void* data, size_t bytes) { return XXH3_64bits(data, bytes); }

}  // namespace gh2render
