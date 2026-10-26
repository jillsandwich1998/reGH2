// Device-level objects of the renderer: the bindless descriptor heaps the translated shaders index, the pipeline layout, samplers, shader modules, the upload ring and barriers.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "impl.h"

#include "builtin_shaders.inc"
#include "debug_shaders.inc"


namespace gh2render {

namespace {

constexpr uint32_t kPushConstantBytes = 24;   // three device addresses: vertex constants, pixel constants, shared constants

VkDescriptorSetLayout MakeHeapLayout(VkDevice dev, VkDescriptorType type) {
  VkDescriptorSetLayoutBinding binding = {};
  binding.binding = 0;
  binding.descriptorType = type;
  binding.descriptorCount = kHeapDescriptors;
  binding.stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS;
  const VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
  VkDescriptorSetLayoutBindingFlagsCreateInfo bf = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
  bf.bindingCount = 1;
  bf.pBindingFlags = &flags;
  VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  ci.pNext = &bf;
  ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
  ci.bindingCount = 1;
  ci.pBindings = &binding;
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  vkCreateDescriptorSetLayout(dev, &ci, nullptr, &layout);
  return layout;
}

// The game draws one mesh in several passes and relies on every pass producing the same depth for the same vertices; the host compiler may fuse the position maths differently
// in every pipeline (it prunes the outputs the pixel shader does not read), so the position is marked Invariant (SPIR-V decoration 18 on the Position built-in).
std::vector<uint32_t> MakePositionInvariant(const uint32_t* code, size_t words) {
  std::vector<uint32_t> out;
  out.reserve(words + 3);
  out.insert(out.end(), code, code + 5);
  bool done = false;
  for (size_t i = 5; i < words;) {
    const uint32_t count = code[i] >> 16, op = code[i] & 0xFFFF;
    if (!count || i + count > words) return {};
    out.insert(out.end(), code + i, code + i + count);
    if (!done && op == 71 /* OpDecorate */ && count == 4 && code[i + 2] == 11 /* BuiltIn */ && code[i + 3] == 0 /* Position */) {
      out.push_back((3u << 16) | 71u);
      out.push_back(code[i + 1]);
      out.push_back(18u);   // Invariant
      done = true;
    }
    i += count;
  }
  return done ? out : std::vector<uint32_t>();
}

}  // namespace

bool Renderer::Impl::InitDeviceObjects() {
  VkDevice dev = vk.device();
  set_layouts[0] = MakeHeapLayout(dev, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
  set_layouts[1] = MakeHeapLayout(dev, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
  set_layouts[2] = MakeHeapLayout(dev, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
  set_layouts[3] = MakeHeapLayout(dev, VK_DESCRIPTOR_TYPE_SAMPLER);
  const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * kHeapDescriptors}, {VK_DESCRIPTOR_TYPE_SAMPLER, kHeapDescriptors}};
  VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  pci.maxSets = 4;
  pci.poolSizeCount = 2;
  pci.pPoolSizes = sizes;
  if (vkCreateDescriptorPool(dev, &pci, nullptr, &heap_pool) != VK_SUCCESS) return false;
  VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = heap_pool;
  ai.descriptorSetCount = 4;
  ai.pSetLayouts = set_layouts;
  if (vkAllocateDescriptorSets(dev, &ai, heap_sets) != VK_SUCCESS) return false;

  const VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushConstantBytes};
  VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 4;
  plci.pSetLayouts = set_layouts;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(dev, &plci, nullptr, &pipeline_layout) != VK_SUCCESS) return false;

  auto make_module = [&](const uint32_t* code, size_t bytes, VkShaderModule* out) {
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = bytes;
    smci.pCode = code;
    return vkCreateShaderModule(dev, &smci, nullptr, out) == VK_SUCCESS;
  };
  if (!make_module(kClearColorVert, sizeof kClearColorVert, &builtin_clear_vs) || !make_module(kRect2dVert, sizeof kRect2dVert, &builtin_rect_vs) ||
      !make_module(kPassthroughFrag, sizeof kPassthroughFrag, &builtin_pass_ps))
    return false;
  if (debug & 128) {
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof kDebugTex0Frag;
    smci.pCode = kDebugTex0Frag;
    vkCreateShaderModule(dev, &smci, nullptr, &debug_tex0_frag);
  }
  if (debug & 512) {
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof kDebugTex1Frag;
    smci.pCode = kDebugTex1Frag;
    vkCreateShaderModule(dev, &smci, nullptr, &debug_tex1_frag);
  }
  if (debug & 16) {
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof kDebugFlatFrag;
    smci.pCode = kDebugFlatFrag;
    vkCreateShaderModule(dev, &smci, nullptr, &debug_flat_frag);
  }
  OpenPipelineCache();

  // fallback resources at index 0 of the heaps: a transparent black texel (what an unset or unsupported texture reads) and the default sampler
  const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!vk.CreateImage(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, usage, 0, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, &dummy_2d)) return false;
  if (!vk.CreateImage(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 6, usage, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_ASPECT_COLOR_BIT, &dummy_cube)) return false;
  VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
  sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sci.maxLod = VK_LOD_CLAMP_NONE;
  if (vkCreateSampler(dev, &sci, nullptr, &default_sampler) != VK_SUCCESS) return false;

  // one-shot: clear the dummies and move them to the sampled layout
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = slots[0].pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer one = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(dev, &cai, &one);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(one, &bi);
  // what an unset texture fetch constant reads on the Xenos is (0, 0, 0, 0) (measured: 70 draws per frame sample such slots in a song; with a white texel they showed as grey boxes)
  const VkClearColorValue white = {{0.f, 0.f, 0.f, 0.f}};
  gh2vk::Image* dummies[2] = {&dummy_2d, &dummy_cube};
  for (gh2vk::Image* img : dummies) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.image = img->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, img->layers};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier2(one, &dep);
    vkCmdClearColorImage(one, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &b.subresourceRange);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(one, &dep);
  }
  vkEndCommandBuffer(one);
  VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cbi.commandBuffer = one;
  VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  si.commandBufferInfoCount = 1;
  si.pCommandBufferInfos = &cbi;
  vkQueueSubmit2(vk.queue(), 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(vk.queue());
  vkFreeCommandBuffers(dev, slots[0].pool, 1, &one);

  WriteImageDescriptor(0, 0, dummy_2d.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  WriteImageDescriptor(2, 0, dummy_cube.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  VkDescriptorImageInfo sinfo = {};
  sinfo.sampler = default_sampler;
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = heap_sets[3];
  w.dstBinding = 0;
  w.dstArrayElement = 0;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  w.pImageInfo = &sinfo;
  vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
  return true;
}

void Renderer::Impl::DestroyDeviceObjects() {
  VkDevice dev = vk.device();
  DestroyOverlay();
  StopPipelineThread();   // saves pipeline_cache.bin and pipelines.list; nothing may build pipelines from here on
  for (auto& kv : pipelines) vkDestroyPipeline(dev, kv.second, nullptr);
  pipelines.clear();
  for (auto& kv : ahead)   // built ahead of time and never asked for
    if (kv.second->pipeline) vkDestroyPipeline(dev, kv.second->pipeline, nullptr);
  ahead.clear();
  for (auto& kv : shader_modules) vkDestroyShaderModule(dev, kv.second, nullptr);
  shader_modules.clear();
  for (auto& kv : samplers) vkDestroySampler(dev, kv.second.first, nullptr);
  samplers.clear();
  for (auto& kv : textures) {
    for (auto& v : kv.second->views) vkDestroyImageView(dev, v.view, nullptr);
    vk.DestroyImage(&kv.second->image);
  }
  textures.clear();
  auto destroy_targets = [&](auto& map) {
    for (auto& kv : map) {
      if (kv.second->sampled_view) vkDestroyImageView(dev, kv.second->sampled_view, nullptr);
      vk.DestroyImage(&kv.second->image);
    }
    map.clear();
  };
  destroy_targets(color_targets);
  destroy_targets(depth_targets);
  destroy_targets(resolved_color);
  destroy_targets(resolved_depth);
  for (auto& t : retired) {
    if (t->sampled_view) vkDestroyImageView(dev, t->sampled_view, nullptr);
    vk.DestroyImage(&t->image);
  }
  retired.clear();
  vk.DestroyImage(&dummy_2d);
  vk.DestroyImage(&dummy_cube);
  if (default_sampler) vkDestroySampler(dev, default_sampler, nullptr);
  for (VkShaderModule m : {builtin_clear_vs, builtin_rect_vs, builtin_pass_ps})
    if (m) vkDestroyShaderModule(dev, m, nullptr);
  if (debug_flat_frag) vkDestroyShaderModule(dev, debug_flat_frag, nullptr);
  if (debug_tex0_frag) vkDestroyShaderModule(dev, debug_tex0_frag, nullptr);
  if (debug_tex1_frag) vkDestroyShaderModule(dev, debug_tex1_frag, nullptr);
  if (pipeline_cache) vkDestroyPipelineCache(dev, pipeline_cache, nullptr);
  if (pipeline_layout) vkDestroyPipelineLayout(dev, pipeline_layout, nullptr);
  if (heap_pool) vkDestroyDescriptorPool(dev, heap_pool, nullptr);
  for (VkDescriptorSetLayout l : set_layouts)
    if (l) vkDestroyDescriptorSetLayout(dev, l, nullptr);
}

uint8_t* Renderer::Impl::RingAlloc(VkDeviceSize size, VkDeviceSize align, VkDeviceSize* offset) {
  FrameSlot& s = *cur;
  const VkDeviceSize start = (s.ring_head + align - 1) & ~(align - 1);
  if (start + size > s.ring.size) {
    ++stats.ring_overflows;
    return nullptr;
  }
  s.ring_head = static_cast<uint32_t>(start + size);
  *offset = start;
  return s.ring.mapped + start;
}

uint32_t Renderer::Impl::AllocHeapIndex(uint32_t heap) {
  if (heap_next[heap] >= kHeapDescriptors) return 0;
  return heap_next[heap]++;
}

void Renderer::Impl::WriteImageDescriptor(uint32_t heap, uint32_t index, VkImageView view, VkImageLayout layout) {
  VkDescriptorImageInfo info = {};
  info.imageView = view;
  info.imageLayout = layout;
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = heap_sets[heap];
  w.dstBinding = 0;
  w.dstArrayElement = index;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  w.pImageInfo = &info;
  vkUpdateDescriptorSets(vk.device(), 1, &w, 0, nullptr);
}

uint32_t Renderer::Impl::GetSamplerIndex(const xenos::TextureFetch& t, float lod_fixed, bool base_only, float lod_bias) {
  // the player's anisotropy option (2..16) is a floor for every filtered texture; t.aniso n means a limit of 2^(n-1)
  const uint32_t forced = opt_anisotropy.load(std::memory_order_relaxed) >= 2 ? static_cast<uint32_t>(std::log2(static_cast<double>(std::min(16u, opt_anisotropy.load())))) + 1 : 0;
  const uint32_t aniso = std::max<uint32_t>(t.aniso, t.min_filter != 0 ? forced : 0);
  const uint32_t key = t.mag_filter | (t.min_filter << 2) | (t.mip_filter << 4) | (aniso << 6) | (t.clamp_x << 9) | (t.clamp_y << 12) | (t.clamp_z << 15) | (base_only ? 1u << 19 : 0u) | (std::min(31u, static_cast<uint32_t>(std::lround(lod_bias * 8.f))) << 27) | (lod_fixed >= 0.f ? (static_cast<uint32_t>(std::lround(lod_fixed * 16.f)) + 1u) << 20 : 0u);
  auto it = samplers.find(key);
  if (it != samplers.end()) return it->second.second;
  VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = t.mag_filter == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  sci.minFilter = t.min_filter == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  sci.mipmapMode = t.mip_filter == 0 ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sci.addressModeU = xenos::AddressMode(t.clamp_x);
  sci.addressModeV = xenos::AddressMode(t.clamp_y);
  sci.addressModeW = xenos::AddressMode(t.clamp_z);
  if (aniso >= 2 && t.min_filter != 0) {
    sci.anisotropyEnable = VK_TRUE;
    sci.maxAnisotropy = std::min(vk.properties().limits.maxSamplerAnisotropy, static_cast<float>(1u << (aniso - 1)));
  }
  static const float bias = LodBias();   // GH2_RENDER_LOD_BIAS: > 0 picks blurrier mip levels for every texture (log2 of the internal scale restores the console's own level choice)
  if (lod_fixed < 0.f) sci.mipLodBias = std::min(std::max(bias, std::round(lod_bias * 8.f) / 8.f), vk.properties().limits.maxSamplerLodBias);
  sci.minLod = 0.f;
  sci.maxLod = t.mip_filter == 2 || base_only ? 0.f : VK_LOD_CLAMP_NONE;   // 2 = base map only
  if (lod_fixed >= 0.f) {   // a snapshot taken at a scaled resolution read by a multi-tap shader: every tap covers the block of texels one guest texel is
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.minLod = sci.maxLod = lod_fixed;
  }
  sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  VkSampler sampler = VK_NULL_HANDLE;
  const uint32_t index = AllocHeapIndex(3);
  if (!index || vkCreateSampler(vk.device(), &sci, nullptr, &sampler) != VK_SUCCESS) return 0;
  VkDescriptorImageInfo info = {};
  info.sampler = sampler;
  VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = heap_sets[3];
  w.dstBinding = 0;
  w.dstArrayElement = index;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  w.pImageInfo = &info;
  vkUpdateDescriptorSets(vk.device(), 1, &w, 0, nullptr);
  samplers[key] = {sampler, index};
  return index;
}

uint32_t Renderer::Impl::DependentMask(uint64_t hash) {
  auto it = dependent_masks.find(hash);
  if (it != dependent_masks.end()) return it->second;
  uint32_t mask = 0;
  if (shader_cache)
    if (auto blob = shader_cache->Find(hash)) mask = blob->dependent_mask;
  dependent_masks[hash] = mask;
  return mask;
}

VkShaderModule Renderer::Impl::GetShaderModule(uint64_t hash, gh2gfx::ShaderStage stage, uint32_t* spec_mask) {
  if (hash == kBuiltinClearVs || hash == kBuiltinRectVs || hash == kBuiltinPassPs) {
    if (spec_mask) *spec_mask = 0;
    return hash == kBuiltinClearVs ? builtin_clear_vs : hash == kBuiltinRectVs ? builtin_rect_vs : builtin_pass_ps;
  }
  if (!shader_cache) return VK_NULL_HANDLE;
  auto blob = shader_cache->Find(hash);
  if (!blob || blob->stage != stage) {
    std::lock_guard<std::mutex> lock(module_mutex);
    if (!missing_logged[hash]) {
      missing_logged[hash] = true;
      ++stats.shader_misses;
      std::fprintf(stderr, "RENDER shader %016llX (%s) is not in the cache\n", (unsigned long long)hash, stage == gh2gfx::ShaderStage::kVertex ? "vs" : "ps");
    }
    return VK_NULL_HANDLE;
  }
  if (spec_mask) *spec_mask = blob->spec_mask;
  {
    std::lock_guard<std::mutex> lock(module_mutex);
    auto it = shader_modules.find(hash);
    if (it != shader_modules.end()) return it->second;
  }
  // made outside the lock (the driver translates the SPIR-V here): the pipeline thread and the command processor may both ask for the same module, one of the two copies is dropped
  SlowTimer timer("shader module", hash);
  std::vector<uint32_t> patched;
  if (stage == gh2gfx::ShaderStage::kVertex && !(debug & 1024)) patched = MakePositionInvariant(blob->spirv, blob->size_bytes / 4);
  VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = patched.empty() ? blob->size_bytes : patched.size() * 4;
  ci.pCode = patched.empty() ? blob->spirv : patched.data();
  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(vk.device(), &ci, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
  std::lock_guard<std::mutex> lock(module_mutex);
  auto inserted = shader_modules.emplace(hash, module);
  if (!inserted.second) {
    vkDestroyShaderModule(vk.device(), module, nullptr);
    return inserted.first->second;
  }
  return module;
}

void Renderer::Impl::ImageBarrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, uint32_t levels) {
  VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = from;
  b.newLayout = to;
  b.image = image;
  b.subresourceRange = {aspect, 0, levels, 0, VK_REMAINING_ARRAY_LAYERS};
  VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &dep);
}

void Renderer::Impl::Transition(TargetImage& t, VkImageLayout to) {
  if (t.layout == to) return;
  ImageBarrier(t.image.image, t.aspect, t.layout, to, t.image.levels);
  t.layout = to;
}

}  // namespace gh2render
