// Render targets and resolves. The Xenos draws into its 10 MB EDRAM and "resolves" (copies) regions of it into texture memory. Here every (base, format) the game renders to is a frame-sized
// image of its own, a resolve copies the region into an image keyed by the destination address, and a texture fetch of that address samples the image: the EDRAM and the round trip through
// guest memory are not emulated. The game splits a frame into tiles (predicated tiling) because the EDRAM is small; the draws of every tile carry absolute screen coordinates, so each tile
// simply lands in its part of the full-size image and the resolve of each tile fills its part of the destination.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "impl.h"

namespace gh2render {

namespace {

VkFormat ColorTargetFormat(uint32_t f) {
  switch (f) {
    case 0: return VK_FORMAT_R8G8B8A8_UNORM;
    case 1: return VK_FORMAT_R8G8B8A8_UNORM;                 // _GAMMA: the shaders and the resolve round trip keep the encoded values
    case 2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;      // 2_10_10_10 viewed as 10_10_10_10
    case 14: return VK_FORMAT_R32_SFLOAT;
    case 15: return VK_FORMAT_R32G32_SFLOAT;
    case 4: case 6: return VK_FORMAT_R16G16_SFLOAT;
    default: return VK_FORMAT_R16G16B16A16_SFLOAT;           // 2_10_10_10_FLOAT, 16_16_16_16(_FLOAT) and anything else
  }
}

float Float20e4To32(uint32_t f24) {
  f24 &= 0xFFFFFF;
  if (!f24) return 0.f;
  uint32_t mantissa = f24 & 0xFFFFF, exponent = f24 >> 20;
  if (!exponent) {   // denormal: normalise
    uint32_t shift = 0;
    while (!(mantissa & 0x100000)) {
      mantissa <<= 1;
      ++shift;
    }
    mantissa &= 0xFFFFF;
    exponent = 1 - shift;
  }
  const uint32_t bits = ((exponent + 112) << 23) | (mantissa << 3);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

}  // namespace

VkSampleCountFlagBits Renderer::Impl::SampleBits(uint32_t samples) { return samples >= 4 ? VK_SAMPLE_COUNT_4_BIT : samples == 2 ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT; }

TargetImage* Renderer::Impl::FindColorTarget(uint32_t base, uint32_t format, uint32_t samples) {
  if (format == 10) format = 2;
  if (format == 12) format = 3;
  auto it = color_targets.find((static_cast<uint64_t>(samples) << 56) | (static_cast<uint64_t>(format) << 32) | base);
  return it == color_targets.end() ? nullptr : it->second.get();
}

TargetImage* Renderer::Impl::FindDepthTarget(uint32_t base, uint32_t format, uint32_t samples) {
  auto it = depth_targets.find((static_cast<uint64_t>(samples) << 56) | (static_cast<uint64_t>(format) << 32) | base);
  return it == depth_targets.end() ? nullptr : it->second.get();
}

TargetImage* Renderer::Impl::GetColorTarget(uint32_t base, uint32_t format, uint32_t samples) {
  // formats that are the same bits seen differently share the EDRAM they alias: the game draws as 2_10_10_10_AS_10_10_10_10 and resolves the same tiles as 2_10_10_10
  if (format == 10) format = 2;
  if (format == 12) format = 3;
  const uint64_t key = (static_cast<uint64_t>(samples) << 56) | (static_cast<uint64_t>(format) << 32) | base;   // one image per sample count: the game reuses the EDRAM with and without MSAA
  auto it = color_targets.find(key);
  if (it != color_targets.end()) return it->second.get();
  SlowTimer timer("color target creation", base);
  auto t = std::make_unique<TargetImage>();
  VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  const uint32_t host = HostSamples(samples);   // the key is the game's sample mode; the image has what the GPU draws with
  if (host > 1) usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;   // multisampled: resolved into a snapshot, never sampled
  if (!vk.CreateImage(ColorTargetFormat(format), frame_width, frame_height, 1, 1, usage, 0, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, &t->image, SampleBits(host))) return nullptr;
  t->width = frame_width;
  t->height = frame_height;
  t->samples = host;
  t->base = base;
  t->format = format;
  if (host > 1) std::fprintf(stderr, "RENDER multisampled colour target: EDRAM base %X format %u, %u samples\n", base, format, host);
  EndRendering();
  Transition(*t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const VkClearColorValue zero = {};
  const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cmd, t->image.image, t->layout, &zero, 1, &range);
  t->clear_state = 1;
  vk.SetName(VK_OBJECT_TYPE_IMAGE, (uint64_t)t->image.image, "color target");
  TargetImage* raw = t.get();
  color_targets[key] = std::move(t);
  return raw;
}

TargetImage* Renderer::Impl::GetDepthTarget(uint32_t base, uint32_t format, uint32_t samples) {
  const uint64_t key = (static_cast<uint64_t>(samples) << 56) | (static_cast<uint64_t>(format) << 32) | base;
  auto it = depth_targets.find(key);
  if (it != depth_targets.end()) return it->second.get();
  auto t = std::make_unique<TargetImage>();
  t->depth = true;
  t->aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  VkImageUsageFlags usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  const uint32_t host = HostSamples(samples);
  if (host > 1) usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!vk.CreateImage(VK_FORMAT_D32_SFLOAT_S8_UINT, frame_width, frame_height, 1, 1, usage, 0, VK_IMAGE_VIEW_TYPE_2D, t->aspect, &t->image, SampleBits(host))) return nullptr;
  t->width = frame_width;
  t->height = frame_height;
  t->samples = host;
  t->base = base;
  t->format = format;
  EndRendering();
  Transition(*t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const VkClearDepthStencilValue clear = {1.f, 0};
  const VkImageSubresourceRange range = {t->aspect, 0, 1, 0, 1};
  vkCmdClearDepthStencilImage(cmd, t->image.image, t->layout, &clear, 1, &range);
  t->clear_state = 1;
  t->clean_value[0] = 1.f;
  vk.SetName(VK_OBJECT_TYPE_IMAGE, (uint64_t)t->image.image, "depth target");
  TargetImage* raw = t.get();
  depth_targets[key] = std::move(t);
  return raw;
}

TargetImage* Renderer::Impl::GetResolved(uint32_t virtual_base, bool depth, VkFormat format, uint32_t width, uint32_t height) {
  auto& map = depth ? resolved_depth : resolved_color;
  auto it = map.find(virtual_base);
  if (it != map.end()) {
    TargetImage* t = it->second.get();
    if (t->image.format == format && t->width == width && t->height == height) return t;
    retired.push_back(std::move(it->second));   // same address, other shape: a new image; the old one may still be read by queued work
    map.erase(it);
  }
  SlowTimer timer("resolved image creation", virtual_base);
  auto t = std::make_unique<TargetImage>();
  t->depth = depth;
  t->aspect = depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (depth) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;   // the resolve of a multisampled depth image is a render pass with a resolve attachment
  // at a scaled resolution a colour snapshot carries a short mip chain (made on demand, see GenerateSnapshotMips): a multi-tap shader's taps are one guest texel apart, which is several of ours
  const uint32_t levels = (!depth && ScaleXf() > 1.001f) ? 1 + static_cast<uint32_t>(std::ceil(std::log2(ScaleXf()))) : 1;
  if (!vk.CreateImage(format, width, height, levels, 1, usage, 0, VK_IMAGE_VIEW_TYPE_2D, t->aspect, &t->image)) return nullptr;
  t->width = width;
  t->height = height;
  vk.SetName(VK_OBJECT_TYPE_IMAGE, (uint64_t)t->image.image, depth ? "resolved depth" : "resolved color");
  if (depth) {   // the attachment-style view covers both aspects; a sampled view must have one
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t->image.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCreateImageView(vk.device(), &vci, nullptr, &t->sampled_view);
  } else {
    t->sampled_view = VK_NULL_HANDLE;
  }
  // a resolve may cover only part of the texture (a tile, a mip level) and the rest must read as zeros, not as whatever the memory held
  EndRendering();
  Transition(*t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const VkImageSubresourceRange clear_range = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (depth) {
    const VkClearDepthStencilValue zero_depth = {0.f, 0};
    vkCmdClearDepthStencilImage(cmd, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero_depth, 1, &clear_range);
  } else {
    const VkClearColorValue zero_color = {};
    vkCmdClearColorImage(cmd, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero_color, 1, &clear_range);
  }
  t->heap_index = AllocHeapIndex(0);
  if (t->heap_index) WriteImageDescriptor(0, t->heap_index, depth ? t->sampled_view : t->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  TargetImage* raw = t.get();
  map[virtual_base] = std::move(t);
  return raw;
}

// The mip chain of a colour snapshot (box filter by blits), for the shaders that read it with several taps.
void Renderer::Impl::GenerateSnapshotMips(TargetImage& t) {
  const uint32_t levels = t.image.levels;
  if (levels < 2) return;
  EndRendering();
  Transition(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  auto barrier = [&](uint32_t base, uint32_t count, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = t.image.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
  };
  barrier(0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  barrier(1, levels - 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  for (uint32_t i = 1; i < levels; ++i) {
    VkImageBlit blit = {};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(std::max(1u, t.width >> (i - 1))), static_cast<int32_t>(std::max(1u, t.height >> (i - 1))), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
    blit.dstOffsets[1] = {static_cast<int32_t>(std::max(1u, t.width >> i)), static_cast<int32_t>(std::max(1u, t.height >> i)), 1};
    vkCmdBlitImage(cmd, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    if (i + 1 < levels) barrier(i, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  }
  barrier(0, levels - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  barrier(levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  t.mips_valid = true;
  ++snapshot_mips_made;
}

TargetImage* Renderer::Impl::FindResolved(uint32_t physical_base) {
  auto it = resolved_color.find(physical_base);
  return it == resolved_color.end() ? nullptr : it->second.get();
}

TargetImage* Renderer::Impl::FindResolvedAny(uint32_t physical_base) {
  if (TargetImage* c = FindResolved(physical_base)) return c;
  auto it = resolved_depth.find(physical_base);
  return it == resolved_depth.end() ? nullptr : it->second.get();
}

void Renderer::Impl::SetClear(TargetImage& t, const float color[4], float depth, uint32_t stencil) {
  float value[4] = {depth, 0.f, 0.f, 0.f};
  if (!t.depth) std::memcpy(value, color, sizeof value);
  static const bool eager = std::getenv("GH2_RENDER_EAGER_CLEAR") != nullptr;   // the previous behaviour, to measure what the lazy clears save
  if (eager) {
    std::memcpy(t.clean_value, value, sizeof value);
    t.clean_stencil = stencil;
    t.clear_state = 2;
    MaterializeClear(t);
    return;
  }
  if (t.clear_state != 0 && std::memcmp(t.clean_value, value, t.depth ? sizeof(float) : sizeof value) == 0 && (!t.depth || t.clean_stencil == stencil)) {
    ++clears_skipped;   // it holds (or is about to hold) exactly this
    return;
  }
  std::memcpy(t.clean_value, value, sizeof value);
  t.clean_stencil = stencil;
  t.clear_state = 2;
  MaterializeClear(t);   // issued right here, in the game's own order: only a clear that changes nothing is dropped
}

void Renderer::Impl::MaterializeClear(TargetImage& t) {
  if (t.clear_state != 2) return;
  EndRendering();
  Transition(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const VkImageSubresourceRange range = {t.aspect, 0, 1, 0, 1};
  if (t.depth) {
    const VkClearDepthStencilValue value = {t.clean_value[0], t.clean_stencil};
    vkCmdClearDepthStencilImage(cmd, t.image.image, t.layout, &value, 1, &range);
  } else {
    VkClearColorValue value = {};
    std::memcpy(value.float32, t.clean_value, sizeof value.float32);
    vkCmdClearColorImage(cmd, t.image.image, t.layout, &value, 1, &range);
  }
  t.clear_state = 1;
}

void Renderer::Impl::BeginRendering(TargetImage* color, TargetImage* depth) {
  // the pass that is open may be on this very target while a clear recorded since (a resolve's, a replay) is still pending: that needs a new pass, which issues the clear first
  const bool pending_clear = (color && color->clear_state == 2) || (depth && depth->clear_state == 2);
  if (pending_clear && rendering && rendering_color == color && rendering_depth == depth) ++clears_reopened;
  if (rendering && rendering_color == color && rendering_depth == depth && !pending_clear) return;
  EndRendering();
  if (color) MaterializeClear(*color);
  if (depth) MaterializeClear(*depth);
  if (color) color->clear_state = 0;
  if (depth) depth->clear_state = 0;
  if (color) Transition(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  if (depth) Transition(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo c = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  VkRenderingAttachmentInfo d = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {frame_width, frame_height}};
  ri.layerCount = 1;
  if (color) {
    c.imageView = color->image.view;
    c.imageLayout = color->layout;
    c.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &c;
  }
  if (depth) {
    d.imageView = depth->image.view;
    d.imageLayout = depth->layout;
    d.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.pDepthAttachment = &d;
    ri.pStencilAttachment = &d;
  }
  if (prof) ProfMark("pass s%u color %X/f%u depth %X", color ? color->samples : (depth ? depth->samples : 1), color ? color->base : 0, color ? color->format : 0, depth ? depth->base : 0);
  vkCmdBeginRendering(cmd, &ri);
  rendering = true;
  rendering_color = color;
  rendering_depth = depth;
  bound_pipeline = VK_NULL_HANDLE;
}

void Renderer::Impl::EndRendering() {
  if (!rendering) return;
  vkCmdEndRendering(cmd);
  rendering = false;
  rendering_color = rendering_depth = nullptr;
}

void Renderer::Impl::Resolve(const uint32_t* regs) {
  namespace r = xenos::reg;
  const uint32_t control = regs[r::RB_COPY_CONTROL];
  const uint32_t src_select = control & 7;
  const bool color_clear = (control >> 8) & 1, depth_clear = (control >> 9) & 1;
  const uint32_t dest_base = regs[r::RB_COPY_DEST_BASE];
  const uint32_t dest_pitch = regs[r::RB_COPY_DEST_PITCH] & 0x3FFF, dest_height = (regs[r::RB_COPY_DEST_PITCH] >> 16) & 0x3FFF;
  const uint32_t dest_format = (regs[r::RB_COPY_DEST_INFO] >> 7) & 0x3F;
  // the region the tile covers, in absolute screen coordinates
  const uint32_t tl = regs[r::PA_SC_WINDOW_SCISSOR_TL], br = regs[r::PA_SC_WINDOW_SCISSOR_BR];
  const uint32_t x0 = std::min(tl & 0x3FFF, kGuestWidth), y0 = std::min((tl >> 16) & 0x3FFF, kGuestHeight);
  const uint32_t x1 = std::min(br & 0x3FFF, std::min(kGuestWidth, dest_pitch)), y1 = std::min((br >> 16) & 0x3FFF, std::min(kGuestHeight, dest_height));
  // the same rectangle in the scaled images (the guest's x0..x1 stay in guest pixels for the texture addressing below)
  const uint32_t sx0 = ScaleX(x0), sy0 = ScaleY(y0), sx1 = ScaleX(x1), sy1 = ScaleY(y1);

  const bool depth_source = src_select == 4;
  if (prof) ProfMark("resolve %s s%u", depth_source ? "depth" : "color", TargetSamples(regs));
  TargetImage* src = nullptr;
  const uint32_t color_info = regs[src_select == 0 || depth_source ? r::RB_COLOR_INFO : r::RB_COLOR1_INFO + (src_select - 1)];
  const uint32_t samples = TargetSamples(regs);
  if (depth_source) src = GetDepthTarget(xenos::DepthBase(regs[r::RB_DEPTH_INFO]), xenos::DepthFormat(regs[r::RB_DEPTH_INFO]), samples);
  else src = GetColorTarget(xenos::ColorBase(color_info), xenos::ColorFormat(color_info), samples);

  static const uint64_t log_resolve_frame = std::getenv("GH2_RENDER_LOG_RESOLVES") ? std::strtoull(std::getenv("GH2_RENDER_LOG_RESOLVES"), nullptr, 10) : ~0ull;
  if (frame == log_resolve_frame)
    std::fprintf(stderr, "RESOLVE f%llu surface %08X (%u samples) src=%s%u fmt %u rect %u,%u-%u,%u -> dst %08X pitch %u height %u dest format %u clear c%d d%d depth_info %08X color_info %08X\n", (unsigned long long)frame, regs[r::RB_SURFACE_INFO], samples,
                 depth_source ? "depth" : "color", src_select, depth_source ? xenos::DepthFormat(regs[r::RB_DEPTH_INFO]) : xenos::ColorFormat(color_info), x0, y0, x1, y1, dest_base, dest_pitch, dest_height,
                 dest_format, color_clear, depth_clear, regs[r::RB_DEPTH_INFO], color_info);
  if (src) MaterializeClear(*src);   // a clear that is still pending would otherwise be missing from the snapshot
  if (src && x1 > x0 && y1 > y0 && dest_pitch && dest_height) {
    // the texture is tiled in 32x32 blocks; a tile of the frame lands at its tile offset inside the same texture, so the texture's own base is the destination minus that offset
    uint32_t bpp = xenos::GetTextureFormat(dest_format).bytes_per_block;
    if (!bpp) bpp = 4;
    const uint32_t row_tiles = (dest_pitch + 31) / 32;
    const uint32_t tile_offset = ((y0 / 32) * row_tiles + x0 / 32) * (32 * 32 * bpp);
    const uint32_t virtual_base = dest_base >= tile_offset ? dest_base - tile_offset : dest_base;
    if (prof && depth_source) {
      std::lock_guard<std::mutex> lock(prof_mutex);
      ++depth_use[virtual_base].first;
    }
    TargetImage* dst = GetResolved(virtual_base, depth_source, src->image.format, ScaleX(std::max(dest_pitch, x1)), ScaleY(std::max(dest_height, y1)));
    if (dst) {
      EndRendering();
      const VkImageAspectFlags aspect = depth_source ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
      if (src->samples > 1 && depth_source) {
        // a multisampled depth image is resolved by the render pass machinery (vkCmdResolveImage only does colour): sample 0 of every pixel of the region, which is what the Xenos' depth copy takes
        Transition(*src, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        Transition(*dst, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo d = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        d.imageView = src->image.view;
        d.imageLayout = src->layout;
        d.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        d.resolveImageView = dst->image.view;
        d.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        d.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{static_cast<int32_t>(sx0), static_cast<int32_t>(sy0)}, {sx1 - sx0, sy1 - sy0}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &d;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdEndRendering(cmd);
      } else if (src->samples > 1) {   // colour: the average of the samples, as the Xenos' resolve does
        Transition(*src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(*dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageResolve resolve = {};
        resolve.srcSubresource = {aspect, 0, 0, 1};
        resolve.dstSubresource = {aspect, 0, 0, 1};
        resolve.srcOffset = {static_cast<int32_t>(sx0), static_cast<int32_t>(sy0), 0};
        resolve.dstOffset = resolve.srcOffset;
        resolve.extent = {sx1 - sx0, sy1 - sy0, 1};
        vkCmdResolveImage(cmd, src->image.image, src->layout, dst->image.image, dst->layout, 1, &resolve);
      } else {
        Transition(*src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(*dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy = {};
        copy.srcSubresource = {aspect, 0, 0, 1};
        copy.dstSubresource = {aspect, 0, 0, 1};
        copy.srcOffset = {static_cast<int32_t>(sx0), static_cast<int32_t>(sy0), 0};
        copy.dstOffset = copy.srcOffset;
        copy.extent = {sx1 - sx0, sy1 - sy0, 1};
        vkCmdCopyImage(cmd, src->image.image, src->layout, dst->image.image, dst->layout, 1, &copy);
      }
      Transition(*dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      dst->resolved_frame = frame;
      dst->mips_valid = false;
    }
  }

  // clears that ride on the resolve
  static const bool keep_contents = std::getenv("GH2_RENDER_NOCLEAR") != nullptr;   // debugging: leave the targets as drawn so that a dump shows them
  if (keep_contents) return;
  if (prof && (color_clear || depth_clear)) ProfMark("resolve clears c%d d%d s%u", color_clear, depth_clear, samples);
  if (color_clear) {
    const uint32_t info = regs[r::RB_COLOR_INFO];
    if (TargetImage* c = GetColorTarget(xenos::ColorBase(info), xenos::ColorFormat(info), samples)) {
      const uint32_t v = regs[r::RB_COLOR_CLEAR];
      VkClearColorValue value = {};
      const uint32_t format = xenos::ColorFormat(info);
      if (format == 0 || format == 1) {
        for (int i = 0; i < 4; ++i) value.float32[i] = ((v >> (8 * i)) & 0xFF) / 255.f;
      } else if (format == 2 || format == 10) {
        for (int i = 0; i < 3; ++i) value.float32[i] = ((v >> (10 * i)) & 0x3FF) / 1023.f;
        value.float32[3] = (v >> 30) / 3.f;
      }
      SetClear(*c, value.float32, 0.f, 0);
    }
  }
  if (depth_clear) {
    const uint32_t info = regs[r::RB_DEPTH_INFO];
    if (TargetImage* d = GetDepthTarget(xenos::DepthBase(info), xenos::DepthFormat(info), samples)) {
      const uint32_t v = regs[r::RB_DEPTH_CLEAR];
      const uint32_t n24 = v >> 8;
      float depth = xenos::DepthFormat(info) ? Float20e4To32(n24) : static_cast<float>(n24 + (n24 >> 23)) * (1.0f / 16777216.0f);
      depth = std::min(1.f, std::max(0.f, depth));
      const float unused[4] = {};
      SetClear(*d, unused, depth, v & 0xFF);
    }
  }
}

}  // namespace gh2render
