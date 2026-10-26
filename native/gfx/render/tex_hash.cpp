#include "tex_hash.h"

#include "shader_cache.h"

namespace gh2render {

uint64_t HashTextureBytes(const void* data, size_t bytes) {
  static const bool avx2 = __builtin_cpu_supports("avx2") != 0;
  return avx2 ? HashBytesAvx2(data, bytes) : gh2gfx::ShaderCache::HashContainer(data, bytes);
}

}  // namespace gh2render
