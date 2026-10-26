// Texture cache: a fetch constant -> the descriptor index the translated shader samples. A fetch constant whose address was the destination of a resolve is served by that image; anything
// else is decoded from guest memory (big endian, tiled) into a Vulkan image: 2D and cube textures with the mip levels the game stores. Layout of a tiled texture's mips (FACT, measured on the
// game's own textures): level 1 starts at the fetch constant's mip address, every level follows the previous one padded to whole 32x32-block tiles, and the levels whose smaller side is 16 texels
// or less share one last packed tile (not read: the image ends at the last level before it). Uncompressed textures get the levels that are not stored from the GPU.
#include <optional>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "impl.h"
#include "tex_hash.h"

namespace gh2render {

namespace {

constexpr uint32_t kGuestMemoryLimit = 0x20000000;   // physical memory: 512 MB
constexpr uint32_t kMaxLevels = 14;

uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

VkComponentSwizzle Swizzle(uint32_t s, VkComponentSwizzle identity) {
  switch (s) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case 4: return VK_COMPONENT_SWIZZLE_ZERO;
    case 5: return VK_COMPONENT_SWIZZLE_ONE;
    default: return identity;
  }
}

void LevelBarrier(VkCommandBuffer cmd, VkImage image, uint32_t base_level, uint32_t count, uint32_t layers, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = from;
  b.newLayout = to;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base_level, count, 0, layers};
  VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

uint32_t Renderer::Impl::PrepareTexture(const uint32_t* f, uint32_t* sampler_index, uint32_t* heap, bool multitap, bool blended) {
  const xenos::TextureFetch t = xenos::DecodeTextureFetch(f);
  const uint32_t dimension = xenos::Bits(f[5], 9, 2);   // 0 1D, 1 2D, 2 3D, 3 cube
  const uint32_t mip_address = xenos::Bits(f[5], 12, 20) << 12;
  // Translucent effects (sparkles, glows, smoke) are drawn as quads of textures whose edges rely on the mip level the console picks at 1280x720: at a scaled resolution the GPU picks a sharper
  // one and the quad's border shows (hard-edged squares around the star power sparkles at 2160p). Their samplers get the bias that restores the console's choice; opaque textures stay sharp.
  static const bool effect_lod = std::getenv("GH2_RENDER_EFFECT_LOD") == nullptr || std::atoi(std::getenv("GH2_RENDER_EFFECT_LOD")) != 0;
  const float effect_bias = (blended && effect_lod && ScaleXf() > 1.001f) ? std::log2(ScaleXf()) : 0.f;
  *sampler_index = GetSamplerIndex(t, -1.f, false, effect_bias);
  *heap = 0;
  auto none = [&](int why) -> uint32_t {   // the fallback texel is read: count why
    ++tex_fallbacks[why];
    return 0;
  };
  if (t.type != 2) return none(0);
  const uint32_t physical = t.base_address << 12;

  if (TargetImage* resolved = FindResolvedAny(physical)) {
    if (prof && resolved->depth) {
      std::lock_guard<std::mutex> lock(prof_mutex);
      ++depth_use[physical].second;
    }
    if (resolved->image.levels > 1 && !resolved->depth) {   // a snapshot at a scaled resolution: the guest only ever reads its level 0, or, with taps a guest texel apart, one guest texel's footprint
      static const bool bias_off = std::getenv("GH2_RENDER_NO_TAP_LOD") != nullptr;   // debugging: the previous behaviour
      if (multitap && !bias_off) {
        if (!resolved->mips_valid) GenerateSnapshotMips(*resolved);
        *sampler_index = GetSamplerIndex(t, std::min(std::log2(ScaleXf()), static_cast<float>(resolved->image.levels - 1)), true);
      } else {
        *sampler_index = GetSamplerIndex(t, -1.f, true);
      }
    }
    return resolved->heap_index ? resolved->heap_index : none(8);
  }
  static const bool log_near = std::getenv("GH2_RENDER_LOG_NEAR") != nullptr;   // (read once: this runs for every texture of every draw)
  if (log_near) {   // debugging: a texture that is not a resolve target although one sits right next to it in memory
    for (const auto* map : {&resolved_color, &resolved_depth})
      for (const auto& kv : *map) {
        const int64_t delta = static_cast<int64_t>(kv.first) - physical;
        static std::unordered_map<uint64_t, bool> reported;
        if (delta != 0 && delta > -0x3000 && delta < 0x3000 && !reported[(static_cast<uint64_t>(physical) << 32) | kv.first]) {
          reported[(static_cast<uint64_t>(physical) << 32) | kv.first] = true;
          std::fprintf(stderr, "RENDER near miss: fetch base %08X (%ux%u fmt %u) vs resolved %08X (%ux%u) delta %lld\n", physical, t.width, t.height, t.format, kv.first, kv.second->width, kv.second->height, (long long)delta);
        }
      }
  }

  auto fallback = [&](const char* why) -> uint32_t {   // the fallback texel is read; say so once per kind of texture
    static std::unordered_map<uint64_t, bool> reported;
    const uint64_t key = (static_cast<uint64_t>(dimension) << 40) | (static_cast<uint64_t>(t.format) << 32) | physical;
    if (!reported[key]) {
      reported[key] = true;
      std::fprintf(stderr, "RENDER texture fallback (%s): base %08X %ux%u dimension %u format %u (%s) tiled %u\n", why, physical, t.width, t.height, dimension, t.format, xenos::GetTextureFormat(t.format).name, t.tiled);
    }
    return none(1);
  };
  if (dimension == 2) return fallback("3D texture");   // not decoded yet
  const bool cube = dimension == 3;
  const uint32_t faces = cube ? 6 : 1;
  if (cube) *heap = 2;
  const xenos::TextureFormatInfo& fi = xenos::GetTextureFormat(t.format);
  if (fi.vk == VK_FORMAT_UNDEFINED || fi.depth) return fallback("format not supported");

  const uint32_t width = t.width, height = dimension == 0 ? 1 : t.height;
  if (width == 0 || height == 0) return none(2);
  // the levels stored in guest memory: level 0 at the base address, then (tiled, not cube) the chain at the mip address
  struct MipLevel {
    uint32_t width, height, width_blocks, height_blocks, pitch_blocks;
    uint64_t source, source_bytes, packed_bytes;   // guest physical address, extent there (tile padding included), decoded size
  };
  auto describe = [&](uint32_t level, uint64_t source, uint32_t pitch_blocks) {
    MipLevel m;
    m.width = std::max(1u, width >> level);
    m.height = std::max(1u, height >> level);
    m.width_blocks = (m.width + fi.block_width - 1) / fi.block_width;
    m.height_blocks = (m.height + fi.block_height - 1) / fi.block_height;
    m.pitch_blocks = std::max(pitch_blocks, m.width_blocks);
    const uint32_t aligned_pitch = AlignUp(m.pitch_blocks, 32), aligned_height = AlignUp(m.height_blocks, 32);
    m.source = source;
    m.source_bytes = t.tiled ? static_cast<uint64_t>(aligned_pitch) * aligned_height * fi.bytes_per_block : static_cast<uint64_t>(m.pitch_blocks) * m.height_blocks * fi.bytes_per_block;
    m.packed_bytes = static_cast<uint64_t>(m.width_blocks) * m.height_blocks * fi.bytes_per_block;
    return m;
  };
  MipLevel stored[kMaxLevels];
  stored[0] = describe(0, physical, (t.pitch_blocks32 * 32) / fi.block_width);
  uint32_t stored_levels = 1;
  if (!cube && t.tiled && t.mip_max > 0 && mip_address) {
    uint64_t at = mip_address;
    for (uint32_t level = 1; level <= t.mip_max && level < kMaxLevels; ++level) {
      if (std::min(std::max(1u, width >> level), std::max(1u, height >> level)) <= 16) break;   // the packed tail starts here
      stored[level] = describe(level, at, 0);
      at += stored[level].source_bytes;
      ++stored_levels;
    }
  }
  const uint64_t face_stride = (stored[0].source_bytes + 4095) / 4096 * 4096;   // cube faces follow each other in memory, each rounded up to 4 KB
  for (uint32_t level = 0; level < stored_levels; ++level)
    if (stored[level].source + stored[level].source_bytes + (level == 0 ? face_stride * (faces - 1) : 0) > kGuestMemoryLimit) return none(2);

  // the image: the stored levels plus, for uncompressed formats, the ones the GPU can still generate
  uint32_t image_levels = stored_levels;
  if (t.mip_max > 0 && !fi.compressed && !cube) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(vk.physical_device(), fi.vk, &props);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((props.optimalTilingFeatures & need) == need) {
      uint32_t full = 1;
      while ((std::max(width, height) >> full) > 0) ++full;
      image_levels = std::max(stored_levels, std::min(t.mip_max + 1, full));
    }
  }

  static const bool no_mips = std::getenv("GH2_TEX_NOMIPS") != nullptr;   // debugging: only the top level of every texture (to tell a mip problem from a decoding one)
  if (no_mips) stored_levels = image_levels = 1;

  // identity of the entry: where the data is, how it is laid out and how it is viewed
  struct Key {
    uint32_t physical, format, width, height, pitch, tiled, endian, mips, faces, mip_address;
  } key_data = {physical, t.format, width, height, stored[0].pitch_blocks, t.tiled, t.endian, stored_levels | (image_levels << 8), faces, stored_levels > 1 ? mip_address : 0};
  const std::string key(reinterpret_cast<const char*>(&key_data), sizeof key_data);

  std::unique_ptr<TextureEntry>& slot = textures[key];
  const bool created = !slot;
  std::optional<SlowTimer> timer;   // only the work that can be slow (a new texture, a changed one) is timed: two clock reads per texture per draw were measurable
  if (created) {
    timer.emplace("texture", physical);
    slot = std::make_unique<TextureEntry>();
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const bool image_ok = vk.CreateImage(fi.vk, width, height, image_levels, faces, usage, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
                                         VK_IMAGE_ASPECT_COLOR_BIT, &slot->image);
    if (!image_ok) {
      textures.erase(key);
      return none(3);
    }
    slot->levels = image_levels;
  }
  TextureEntry& e = *slot;
  e.used_frame = frame;

  if (created || e.checked_frame != frame) {
    e.checked_frame = frame;
    const auto hash_start = std::chrono::steady_clock::now();
    uint64_t hash = 1469598103934665603ull;   // has the guest rewritten any level?
    for (uint32_t level = 0; level < stored_levels; ++level)
      hash = (hash ^ HashTextureBytes(config.memory.Physical(static_cast<uint32_t>(stored[level].source)),
                                                        static_cast<size_t>(stored[level].source_bytes + (level == 0 ? face_stride * (faces - 1) : 0)))) * 1099511628211ull;
    const auto decode_start = std::chrono::steady_clock::now();
    tex_hash_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(decode_start - hash_start).count());
    if (created || hash != e.hash) {
      if (!timer) timer.emplace("texture", physical);
      VkDeviceSize level_offset[kMaxLevels] = {};
      VkDeviceSize need = 0;
      for (uint32_t level = 0; level < stored_levels; ++level) {
        level_offset[level] = need;
        need = AlignUp(static_cast<uint32_t>(need + stored[level].packed_bytes * (level == 0 ? faces : 1)), 16);
      }
      VkDeviceSize offset = 0;
      uint8_t* staging = RingAlloc(need, 16, &offset);
      if (!staging) return none(4);   // out of staging space this frame: the next frame retries (the entry stays "changed")
      // decode in ordinary memory and copy the result to the staging area: the ring is write-combined memory, reading it back (the endian swap does) runs at a few MB/s
      thread_local std::vector<uint8_t> decoded;
      decoded.resize(static_cast<size_t>(need));
      for (uint32_t level = 0; level < stored_levels; ++level) {
        const MipLevel& m = stored[level];
        const uint8_t* source = config.memory.Physical(static_cast<uint32_t>(m.source));
        for (uint32_t face = 0; face < (level == 0 ? faces : 1); ++face)
          xenos::DecodeLevel2D(source + face * face_stride, m.width_blocks, m.height_blocks, m.pitch_blocks, t.tiled != 0, t.endian, fi, decoded.data() + level_offset[level] + face * m.packed_bytes);
      }
      std::memcpy(staging, decoded.data(), decoded.size());
      tex_decode_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - decode_start).count());
      tex_decode_bytes += decoded.size();
      e.hash = hash;
      ++stats.textures_uploaded;
      static const char* tex_dump = std::getenv("GH2_RENDER_DUMP_TEX");   // debugging: the decoded top level of the first textures, for tools/ to turn into pictures
      static int tex_dumped = 0;
      static const int tex_dump_format = std::getenv("GH2_RENDER_DUMP_TEX_FMT") ? std::atoi(std::getenv("GH2_RENDER_DUMP_TEX_FMT")) : -1;   // only textures of this format (a Xenos texture format number)
      if (tex_dump && tex_dumped < 600 && (tex_dump_format < 0 || static_cast<int>(t.format) == tex_dump_format) && !(std::getenv("GH2_RENDER_DUMP_TEX_NODXT") && (t.format == 18 || t.format == 20 || t.format == 49 || t.format == 19))) {
        char path[512];
        std::snprintf(path, sizeof path, "%s_%03d_%08X_%ux%u_fmt%u_t%u_e%u_%s_mips%u.bin", tex_dump, tex_dumped++, physical, width, height, t.format, t.tiled, t.endian, fi.name, stored_levels);
        if (FILE* f = std::fopen(path, "wb")) {
          std::fwrite(decoded.data(), 1, static_cast<size_t>(stored[0].packed_bytes * faces), f);
          std::fclose(f);
        }
      }

      EndRendering();
      LevelBarrier(cmd, e.image.image, 0, e.levels, faces, created ? VK_IMAGE_LAYOUT_UNDEFINED : e.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkBufferImageCopy regions[kMaxLevels + 5] = {};
      uint32_t region_count = 0;
      for (uint32_t level = 0; level < stored_levels; ++level)
        for (uint32_t face = 0; face < (level == 0 ? faces : 1); ++face) {
          VkBufferImageCopy& r = regions[region_count++];
          r.bufferOffset = offset + level_offset[level] + face * stored[level].packed_bytes;
          r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1};
          r.imageExtent = {stored[level].width, stored[level].height, 1};
        }
      vkCmdCopyBufferToImage(cmd, cur->ring.buffer, e.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region_count, regions);
      if (e.levels > stored_levels) {   // the last stored level feeds a blit chain for the rest
        if (stored_levels > 1) LevelBarrier(cmd, e.image.image, 0, stored_levels - 1, faces, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        int32_t w = stored[stored_levels - 1].width, h = stored[stored_levels - 1].height;
        for (uint32_t i = stored_levels; i < e.levels; ++i) {
          LevelBarrier(cmd, e.image.image, i - 1, 1, faces, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
          VkImageBlit blit = {};
          blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
          blit.srcOffsets[1] = {w, h, 1};
          w = std::max(1, w / 2);
          h = std::max(1, h / 2);
          blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
          blit.dstOffsets[1] = {w, h, 1};
          vkCmdBlitImage(cmd, e.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, e.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
          LevelBarrier(cmd, e.image.image, i - 1, 1, faces, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        LevelBarrier(cmd, e.image.image, e.levels - 1, 1, faces, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      } else {
        LevelBarrier(cmd, e.image.image, 0, e.levels, faces, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      }
      e.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
  }
  if (e.layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) return none(5);   // never uploaded (staging was full)

  // the view for this swizzle
  for (const TextureEntry::View& v : e.views)
    if (v.swizzle == t.swizzle) return v.index;
  const uint32_t index = AllocHeapIndex(cube ? 2 : 0);
  if (!index) return none(6);
  VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = e.image.image;
  vci.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
  vci.format = fi.vk;
  vci.components.r = Swizzle(t.swizzle & 7, VK_COMPONENT_SWIZZLE_R);
  vci.components.g = Swizzle((t.swizzle >> 3) & 7, VK_COMPONENT_SWIZZLE_G);
  vci.components.b = Swizzle((t.swizzle >> 6) & 7, VK_COMPONENT_SWIZZLE_B);
  vci.components.a = Swizzle((t.swizzle >> 9) & 7, VK_COMPONENT_SWIZZLE_A);
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, e.levels, 0, faces};
  VkImageView view = VK_NULL_HANDLE;
  if (vkCreateImageView(vk.device(), &vci, nullptr, &view) != VK_SUCCESS) return none(7);
  WriteImageDescriptor(cube ? 2 : 0, index, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  e.views.push_back({t.swizzle, view, index});
  return index;
}

}  // namespace gh2render
