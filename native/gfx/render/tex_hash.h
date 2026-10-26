// The hash the texture cache uses to notice that the guest rewrote a texture (every texture a draw uses, every frame): XXH3 64 bit, with the AVX2 version of the library when the
// processor has it (twice the speed on cache-resident data; the same values as the portable version, so nothing depends on which one ran).
#pragma once

#include <cstddef>
#include <cstdint>

namespace gh2render {

uint64_t HashTextureBytes(const void* data, size_t bytes);
uint64_t HashBytesAvx2(const void* data, size_t bytes);   // tex_hash_avx2.cpp, compiled with -mavx2: only call it when the processor has AVX2

}  // namespace gh2render
