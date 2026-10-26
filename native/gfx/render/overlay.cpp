// The on-screen menu and the options the player changes while the game runs.
//
// The application records its menu (Dear ImGui) as draw lists and hands them over with OverlaySubmit; the renderer draws the latest list over the finished picture, straight into the
// swapchain image, at the window's resolution (not the internal one: text stays sharp whatever the game renders at). Textures arrive as RGBA8 from any thread and are uploaded on the
// render thread, with the frame's upload ring as staging. Nothing here is on the game's path when no menu is open: without a frame the swapchain image goes to the presenter as before.
#include <algorithm>
#include <cmath>

#include "impl.h"

#include "builtin_shaders.inc"

namespace gh2render {

// ------------------------------------------------------------------------------------------------------------------------------- public interface

uint32_t Renderer::OverlayCreateTexture(uint32_t width, uint32_t height, bool linear, const uint8_t* rgba) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.overlay_mutex);
  const uint32_t id = s.overlay_next_id++;
  Impl::OverlayTexture& t = s.overlay_textures[id];
  t.width = width;
  t.height = height;
  t.linear = linear;
  t.pixels.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
  return id;
}

void Renderer::OverlayDestroyTexture(uint32_t id) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.overlay_mutex);
  if (id) s.overlay_dead.push_back(id);
}

void Renderer::OverlaySubmit(std::shared_ptr<const OverlayFrame> frame) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.overlay_mutex);
  s.overlay_frame = std::move(frame);
}

void Renderer::RequestScreenshot(std::function<void(uint32_t, uint32_t, std::vector<uint8_t>)> done) {
  Impl& s = *impl_;
  {
    std::lock_guard<std::mutex> lock(s.shot_mutex);
    s.shot_callbacks.push_back(std::move(done));
  }
  s.shot_after = s.frame;
  s.shot_log_frame = s.frame + 1;   // the draws of the frame whose picture is saved go to the log (GH2_RENDER_LOG_FRAME machinery)
  s.shot_pending = true;
}

void Renderer::ReloadShaderCache(const std::string& dir) {
  Impl& s = *impl_;
  {
    std::lock_guard<std::mutex> lock(s.reload_mutex);
    s.reload_dir = dir;
  }
  s.reload_pending = true;
}

void Renderer::Impl::ApplyShaderReload() {
  if (!reload_pending.exchange(false)) return;
  std::string dir;
  {
    std::lock_guard<std::mutex> lock(reload_mutex);
    dir = reload_dir;
  }
  std::string error;
  auto fresh = gh2gfx::ShaderCache::Open(dir, &error);
  if (!fresh) {
    std::fprintf(stderr, "RENDER shader cache reload: %s\n", error.c_str());
    return;
  }
  shader_cache = std::move(fresh);
  dependent_masks.clear();   // masks read while there was no cache are all zero
  {
    std::lock_guard<std::mutex> lock(module_mutex);
    missing_logged.clear();
  }
  std::fprintf(stderr, "RENDER shader cache reloaded: %zu shaders\n", shader_cache->size());
  StartPipelineThread();
}

void Renderer::SetOptions(const RenderOptions& options) {
  Impl& s = *impl_;
  s.opt_height = std::min(4320u, std::max(360u, options.height));
  s.opt_msaa = options.msaa;
  s.opt_anisotropy = options.anisotropy;
  s.opt_present = std::min(2u, options.present_mode);
}

RenderOptions Renderer::GetOptions() const {
  const Impl& s = *impl_;
  RenderOptions o;
  o.height = s.opt_height;
  o.msaa = s.opt_msaa;
  o.anisotropy = s.opt_anisotropy;
  o.present_mode = s.opt_present;
  return o;
}

// ------------------------------------------------------------------------------------------------------------------------------- options

void Renderer::Impl::DestroyTargets() {
  VkDevice dev = vk.device();
  vkDeviceWaitIdle(dev);
  rendering = false;
  rendering_color = rendering_depth = nullptr;
  auto destroy = [&](auto& map) {
    for (auto& kv : map) {
      if (kv.second->sampled_view) vkDestroyImageView(dev, kv.second->sampled_view, nullptr);
      vk.DestroyImage(&kv.second->image);
    }
    map.clear();
  };
  destroy(color_targets);
  destroy(depth_targets);
  destroy(resolved_color);
  destroy(resolved_depth);
  for (auto& t : retired) {
    if (t->sampled_view) vkDestroyImageView(dev, t->sampled_view, nullptr);
    vk.DestroyImage(&t->image);
  }
  retired.clear();
}

void Renderer::Impl::ApplyOptions() {
  if (!vk_ready) return;
  const bool msaa = opt_msaa;
  if (msaa != MsaaFlag().load()) {
    MsaaFlag() = msaa;
    DestroyTargets();   // the images were made with the other sample count
    std::fprintf(stderr, "RENDER option: multisampling %s\n", msaa ? "on" : "off");
  }
  vk.SetPresentMode(static_cast<int>(opt_present.load()));
  const uint32_t h = opt_height;
  if (h != frame_height) {
    DestroyTargets();
    frame_height = h;
    frame_width = (h * 16 + 4) / 9;
    std::fprintf(stderr, "RENDER option: internal resolution %ux%u (scale %.3f)\n", frame_width, frame_height, h / 720.0);
  }
}

// ------------------------------------------------------------------------------------------------------------------------------- drawing

bool Renderer::Impl::InitOverlay(VkFormat format) {
  VkDevice dev = vk.device();
  overlay_format = format;
  VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo dlci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dlci.bindingCount = 1;
  dlci.pBindings = &binding;
  if (vkCreateDescriptorSetLayout(dev, &dlci, nullptr, &overlay_set_layout) != VK_SUCCESS) return false;
  VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512};
  VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  dpci.maxSets = 512;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &size;
  if (vkCreateDescriptorPool(dev, &dpci, nullptr, &overlay_pool) != VK_SUCCESS) return false;
  VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
  VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &overlay_set_layout;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &push;
  if (vkCreatePipelineLayout(dev, &plci, nullptr, &overlay_pipeline_layout) != VK_SUCCESS) return false;
  for (int i = 0; i < 2; ++i) {
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &sci, nullptr, &overlay_samplers[i]) != VK_SUCCESS) return false;
  }
  auto make_module = [&](const uint32_t* code, size_t bytes, VkShaderModule* out) {
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = bytes;
    smci.pCode = code;
    return vkCreateShaderModule(dev, &smci, nullptr, out) == VK_SUCCESS;
  };
  if (!make_module(kOverlayVert, sizeof kOverlayVert, &overlay_vs) || !make_module(kOverlayFrag, sizeof kOverlayFrag, &overlay_fs)) return false;

  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = overlay_vs;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = overlay_fs;
  stages[1].pName = "main";
  VkVertexInputBindingDescription vb = {0, sizeof(OverlayVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription va[3] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlayVertex, x)},
                                             {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(OverlayVertex, u)},
                                             {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(OverlayVertex, color)}};
  VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &vb;
  vi.vertexAttributeDescriptionCount = 3;
  vi.pVertexAttributeDescriptions = va;
  VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.f;
  VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState att = {};
  att.blendEnable = VK_TRUE;
  att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  att.colorBlendOp = VK_BLEND_OP_ADD;
  att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;   // the window stays opaque
  att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  att.alphaBlendOp = VK_BLEND_OP_ADD;
  att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &att;
  const VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dynamic;
  VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = 1;
  ri.pColorAttachmentFormats = &format;
  VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.pNext = &ri;
  gp.stageCount = 2;
  gp.pStages = stages;
  gp.pVertexInputState = &vi;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &ds;
  gp.layout = overlay_pipeline_layout;
  if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &overlay_pipeline) != VK_SUCCESS) return false;

  OverlayTexture& white = overlay_textures[0];   // the texture of flat-coloured shapes
  white.width = white.height = 1;
  white.linear = false;
  white.pixels.assign(4, 255);
  return true;
}

void Renderer::Impl::DestroyOverlay() {
  VkDevice dev = vk.device();
  for (auto& kv : overlay_textures) {
    if (kv.second.image.view) {
      vkFreeDescriptorSets(dev, overlay_pool, 1, &kv.second.set);
      vk.DestroyImage(&kv.second.image);
    }
  }
  overlay_textures.clear();
  if (overlay_pipeline) vkDestroyPipeline(dev, overlay_pipeline, nullptr);
  if (overlay_pipeline_layout) vkDestroyPipelineLayout(dev, overlay_pipeline_layout, nullptr);
  if (overlay_pool) vkDestroyDescriptorPool(dev, overlay_pool, nullptr);
  if (overlay_set_layout) vkDestroyDescriptorSetLayout(dev, overlay_set_layout, nullptr);
  for (VkSampler sampler : overlay_samplers)
    if (sampler) vkDestroySampler(dev, sampler, nullptr);
  for (VkShaderModule m : {overlay_vs, overlay_fs})
    if (m) vkDestroyShaderModule(dev, m, nullptr);
  overlay_pipeline = VK_NULL_HANDLE;
  overlay_pipeline_layout = VK_NULL_HANDLE;
  overlay_pool = VK_NULL_HANDLE;
  overlay_set_layout = VK_NULL_HANDLE;
  overlay_samplers[0] = overlay_samplers[1] = VK_NULL_HANDLE;
  overlay_vs = overlay_fs = VK_NULL_HANDLE;
  vk.DestroyBuffer(&final_buffer);
}

VkImageLayout Renderer::Impl::DrawOverlay(const gh2vk::Context::Frame& target, VkExtent2D extent) {
  const VkImageLayout untouched = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  std::lock_guard<std::mutex> lock(overlay_mutex);
  if (!overlay_pipeline) {
    if (overlay_failed || !InitOverlay(vk.swapchain_format())) {
      if (!overlay_failed) std::fprintf(stderr, "RENDER overlay: cannot create the menu pipeline\n");
      overlay_failed = true;
      return untouched;
    }
  }
  VkDevice dev = vk.device();
  if (!overlay_dead.empty()) {   // the menu released textures: the frames in flight may still read them
    vkDeviceWaitIdle(dev);
    for (uint32_t id : overlay_dead) {
      auto it = overlay_textures.find(id);
      if (it == overlay_textures.end()) continue;
      if (it->second.image.view) {
        vkFreeDescriptorSets(dev, overlay_pool, 1, &it->second.set);
        vk.DestroyImage(&it->second.image);
      }
      overlay_textures.erase(it);
    }
    overlay_dead.clear();
  }
  // textures that arrived since the last frame
  for (auto& kv : overlay_textures) {
    OverlayTexture& t = kv.second;
    if (t.pixels.empty()) continue;
    VkDeviceSize offset = 0;
    uint8_t* staging = RingAlloc(t.pixels.size(), 16, &offset);
    if (!staging) continue;   // next frame
    if (!vk.CreateImage(VK_FORMAT_R8G8B8A8_UNORM, t.width, t.height, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, &t.image)) continue;
    std::memcpy(staging, t.pixels.data(), t.pixels.size());
    ImageBarrier(t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region = {};
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {t.width, t.height, 1};
    vkCmdCopyBufferToImage(cmd, cur->ring.buffer, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ImageBarrier(t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = overlay_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &overlay_set_layout;
    if (vkAllocateDescriptorSets(dev, &ai, &t.set) != VK_SUCCESS) {
      vk.DestroyImage(&t.image);
      continue;
    }
    VkDescriptorImageInfo info = {overlay_samplers[t.linear ? 1 : 0], t.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = t.set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    t.pixels.clear();
    t.pixels.shrink_to_fit();
  }
  const std::shared_ptr<const OverlayFrame> frame = overlay_frame;
  if (!frame || frame->draws.empty() || frame->vertices.empty() || frame->width <= 0 || frame->height <= 0) return untouched;
  VkDeviceSize voffset = 0, ioffset = 0;
  uint8_t* vdst = RingAlloc(frame->vertices.size() * sizeof(OverlayVertex), 16, &voffset);
  uint8_t* idst = vdst ? RingAlloc(frame->indices.size() * sizeof(uint16_t), 4, &ioffset) : nullptr;
  if (!vdst || !idst) return untouched;
  std::memcpy(vdst, frame->vertices.data(), frame->vertices.size() * sizeof(OverlayVertex));
  std::memcpy(idst, frame->indices.data(), frame->indices.size() * sizeof(uint16_t));

  ImageBarrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = target.view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, extent};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &color;
  vkCmdBeginRendering(cmd, &ri);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlay_pipeline);
  const VkViewport viewport = {0.f, 0.f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.f, 1.f};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  const float push[2] = {2.f / frame->width, 2.f / frame->height};
  vkCmdPushConstants(cmd, overlay_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
  const VkBuffer vbuffer = cur->ring.buffer;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vbuffer, &voffset);
  vkCmdBindIndexBuffer(cmd, cur->ring.buffer, ioffset, VK_INDEX_TYPE_UINT16);
  const float sx = extent.width / frame->width, sy = extent.height / frame->height;
  uint32_t bound_texture = ~0u;
  for (const OverlayDraw& d : frame->draws) {
    if (!d.index_count) continue;
    auto it = overlay_textures.find(d.texture);
    if (it == overlay_textures.end() || !it->second.set) it = overlay_textures.find(0);
    if (it == overlay_textures.end() || !it->second.set) continue;   // not even the white texel is up yet
    const int32_t x0 = std::max(0, static_cast<int32_t>(std::floor(d.clip[0] * sx))), y0 = std::max(0, static_cast<int32_t>(std::floor(d.clip[1] * sy)));
    const int32_t x1 = std::min<int32_t>(extent.width, static_cast<int32_t>(std::ceil(d.clip[2] * sx))), y1 = std::min<int32_t>(extent.height, static_cast<int32_t>(std::ceil(d.clip[3] * sy)));
    if (x1 <= x0 || y1 <= y0) continue;
    const VkRect2D scissor = {{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    if (bound_texture != d.texture) {
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlay_pipeline_layout, 0, 1, &it->second.set, 0, nullptr);
      bound_texture = d.texture;
    }
    vkCmdDrawIndexed(cmd, d.index_count, 1, d.index_offset, d.base_vertex, 0);
  }
  vkCmdEndRendering(cmd);
  return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

// ------------------------------------------------------------------------------------------------------------------------------- debugging: the window as presented

void Renderer::Impl::PrepareFinalDump(VkExtent2D extent, VkImage image, VkImageLayout* layout) {
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
  if (!final_buffer.buffer || final_buffer.size < bytes) {
    vkDeviceWaitIdle(vk.device());
    vk.DestroyBuffer(&final_buffer);
    if (!vk.CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &final_buffer)) return;
  }
  ImageBarrier(image, VK_IMAGE_ASPECT_COLOR_BIT, *layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  *layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkBufferImageCopy region = {};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {extent.width, extent.height, 1};
  vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, final_buffer.buffer, 1, &region);
  final_extent = extent;
  final_pending = true;
}

void Renderer::Impl::WriteFinalDump() {   // after the frame's fence has been waited for
  if (!final_pending || !final_buffer.mapped) return;
  final_pending = false;
  char path[512];
  std::snprintf(path, sizeof path, "%s_%06llu_final.ppm", dump_prefix.c_str(), (unsigned long long)frame);
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  const size_t pixels = static_cast<size_t>(final_extent.width) * final_extent.height;
  std::fprintf(f, "P6\n%u %u\n255\n", final_extent.width, final_extent.height);
  std::vector<uint8_t> rgb(pixels * 3);
  const bool bgra = vk.swapchain_format() == VK_FORMAT_B8G8R8A8_UNORM || vk.swapchain_format() == VK_FORMAT_B8G8R8A8_SRGB;
  for (size_t i = 0; i < pixels; ++i) {
    const uint8_t* p = final_buffer.mapped + 4 * i;
    rgb[3 * i] = bgra ? p[2] : p[0];
    rgb[3 * i + 1] = p[1];
    rgb[3 * i + 2] = bgra ? p[0] : p[2];
  }
  std::fwrite(rgb.data(), 1, rgb.size(), f);
  std::fclose(f);
  std::fprintf(stderr, "RENDER dumped the presented window of frame %llu to %s\n", (unsigned long long)frame, path);
}

}  // namespace gh2render
