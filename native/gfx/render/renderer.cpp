// Lifecycle of the native renderer: Vulkan bring-up, frame slots, the swap (present) and the Sink entry points. The work itself is in device.cpp, targets.cpp, textures.cpp, draw.cpp.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "impl.h"

namespace gh2render {

namespace {
using Clock = std::chrono::steady_clock;
uint64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
}  // namespace

Renderer::Impl::~Impl() {
  if (vk_ready) {
    vkDeviceWaitIdle(vk.device());
    DestroyDeviceObjects();
    vk.DestroyBuffer(&dump_buffer);
    if (ts_pool) vkDestroyQueryPool(vk.device(), ts_pool, nullptr);
    if (prof_pool) vkDestroyQueryPool(vk.device(), prof_pool, nullptr);
    for (auto& s : slots) {
      vk.DestroyBuffer(&s.ring);
      vkDestroySemaphore(vk.device(), s.acquired, nullptr);
      vkDestroyFence(vk.device(), s.fence, nullptr);
      vkDestroyCommandPool(vk.device(), s.pool, nullptr);
    }
  }
  if (trace) std::fclose(trace);
}

void Renderer::Impl::Tracef(const char* fmt, ...) {
  if (!trace) return;
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(trace, fmt, ap);
  va_end(ap);
}

bool Renderer::Impl::InitVulkan() {
  if (vk_ready) return true;
  if (vk_failed) return false;
  std::string error;
  if (!vk.Initialize(config.window, &error)) {
    std::fprintf(stderr, "RENDER vulkan: %s\n", error.c_str());
    vk_failed = true;
    return false;
  }
  VkDevice dev = vk.device();
  for (auto& s : slots) {
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = vk.queue_family();
    vkCreateCommandPool(dev, &pci, nullptr, &s.pool);
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = s.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &ai, &s.cmd);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(dev, &fci, nullptr, &s.fence);
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(dev, &sci, nullptr, &s.acquired);
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!vk.CreateBuffer(kRingBytes, usage, true, &s.ring)) {
      std::fprintf(stderr, "RENDER vulkan: cannot allocate the %llu MB upload ring\n", (unsigned long long)(kRingBytes >> 20));
      vk_failed = true;
      return false;
    }
  }
  std::string cache_error;
  shader_cache = gh2gfx::ShaderCache::Open(config.shader_cache_dir, &cache_error);
  if (!shader_cache) std::fprintf(stderr, "RENDER shader cache: %s (nothing can be drawn)\n", cache_error.c_str());
  if (!InitDeviceObjects()) {
    vk_failed = true;
    return false;
  }
  VkQueryPoolCreateInfo qpci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qpci.queryCount = 2 * kFramesInFlight;
  vkCreateQueryPool(dev, &qpci, nullptr, &ts_pool);
  if (prof) {
    qpci.queryCount = kProfMax * kFramesInFlight;
    vkCreateQueryPool(dev, &qpci, nullptr, &prof_pool);
  }
  vk_ready = true;
  std::fprintf(stderr, "RENDER vulkan ready on %s (%zu shaders in the cache)\n", vk.device_name().c_str(), shader_cache ? shader_cache->size() : size_t(0));
  if (shader_cache) StartPipelineThread();   // builds the pipelines of earlier runs while the game boots
  return true;
}

bool Renderer::Impl::BeginFrame() {
  if (cur) return true;
  if (!InitVulkan()) return false;
  ApplyShaderReload();
  ApplyOptions();
  FrameSlot& s = slots[slot_index];
  VkDevice dev = vk.device();
  {
    SlowTimer timer("wait for the frame slot's fence", slot_index);
    vkWaitForFences(dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
  }
  vkResetFences(dev, 1, &s.fence);
  vkResetCommandBuffer(s.cmd, 0);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(s.cmd, &bi);
  const uint32_t index = static_cast<uint32_t>(&s - slots);
  if (ts_valid[index]) {   // the GPU time of the frame this slot recorded last time (its fence has been waited for)
    uint64_t t[2];
    if (vkGetQueryPoolResults(dev, ts_pool, 2 * index, 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      const uint64_t ns = static_cast<uint64_t>((t[1] - t[0]) * vk.properties().limits.timestampPeriod);
      gpu_ns_sum += ns;
      ++gpu_frames;
      if (ns > gpu_ns_max) gpu_ns_max = ns;
    }
  }
  vkResetQueryPool(dev, ts_pool, 2 * index, 2);
  vkCmdWriteTimestamp(s.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ts_pool, 2 * index);
  if (prof) ProfCollect(index);
  ts_valid[index] = true;
  uint64_t used = s.ring_head;
  if (used > stats.ring_peak_bytes) stats.ring_peak_bytes = used;
  s.ring_head = 0;
  cur = &s;
  cmd = s.cmd;
  ProfMark("misc");
  // the bindless heaps are bound for the whole frame
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 4, heap_sets, 0, nullptr);
  bound_pipeline = VK_NULL_HANDLE;
  last_vs_address = last_ps_address = 0;
  rendering = false;
  rendering_color = rendering_depth = nullptr;
  return true;
}

void Renderer::Impl::SubmitFrame(bool present, const TargetImage* front) {
  if (!cur) return;
  EndRendering();
  ProfMark("end");
  FrameSlot& slot = *cur;
  const bool env_dump = dump_every && dump_count < dump_max && frame >= dump_from && frame % dump_every == 0;
  bool shot = false;
  if (shot_pending.load() && front && front->image.image && frame >= shot_after.load() + 2) {   // `frame` already counts this swap: the picture of frame shot_after + 1
    std::lock_guard<std::mutex> lock(shot_mutex);
    shot = !shot_callbacks.empty();
    if (shot_callbacks.size() <= 1) shot_pending = false;
  }
  const bool dumping = env_dump || shot;
  gh2vk::Context::Frame target;
  const bool have_target = present && vk.AcquireNext(slot.acquired, &target);
  if (have_target) {
    const VkExtent2D ext = vk.swapchain_extent();
    ImageBarrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue black = {{0.f, 0.f, 0.f, 1.f}};
    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    if (front && front->image.image && front->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && !front->depth) {
      // letterbox: the picture keeps its aspect ratio inside the window
      const float sx = static_cast<float>(ext.width) / front->width, sy = static_cast<float>(ext.height) / front->height;
      const float scale = std::min(sx, sy);
      const int32_t dw = static_cast<int32_t>(front->width * scale), dh = static_cast<int32_t>(front->height * scale);
      const int32_t dx = (static_cast<int32_t>(ext.width) - dw) / 2, dy = (static_cast<int32_t>(ext.height) - dh) / 2;
      ImageBarrier(front->image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkImageBlit blit = {};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.srcOffsets[1] = {static_cast<int32_t>(front->width), static_cast<int32_t>(front->height), 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.dstOffsets[0] = {dx, dy, 0};
      blit.dstOffsets[1] = {dx + dw, dy + dh, 1};
      vkCmdBlitImage(cmd, front->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
      ImageBarrier(front->image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    VkImageLayout layout = DrawOverlay(target, ext);
    if (env_dump && dump_final) PrepareFinalDump(ext, target.image, &layout);
    ImageBarrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  }
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts_pool, 2 * static_cast<uint32_t>(&slot - slots) + 1);
  vkEndCommandBuffer(cmd);

  VkSemaphoreSubmitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  wait.semaphore = slot.acquired;
  wait.stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
  VkSemaphoreSubmitInfo signal = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  if (have_target) signal.semaphore = vk.RenderedSemaphore(target.index);
  signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  VkCommandBufferSubmitInfo cb = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cb.commandBuffer = cmd;
  VkSubmitInfo2 submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.waitSemaphoreInfoCount = have_target ? 1 : 0;
  submit.pWaitSemaphoreInfos = &wait;
  submit.commandBufferInfoCount = 1;
  submit.pCommandBufferInfos = &cb;
  submit.signalSemaphoreInfoCount = have_target ? 1 : 0;
  submit.pSignalSemaphoreInfos = &signal;
  {
    SlowTimer timer("queue submit");
    vkQueueSubmit2(vk.queue(), 1, &submit, slot.fence);
  }
  if (have_target) {
    SlowTimer timer("present");
    if (vk.Present(target, vk.RenderedSemaphore(target.index))) ++stats.frames_presented;
    else ++stats.frames_dropped;
  } else {
    ++stats.frames_dropped;
  }
  stats.gpu_memory_bytes = vk.DeviceMemoryInUse();
  if (dumping) {
    vkWaitForFences(vk.device(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
    if (shot) {
      std::vector<uint8_t> rgb;
      dump_out = &rgb;
      DumpImage(*front, "screenshot");
      dump_out = nullptr;
      std::function<void(uint32_t, uint32_t, std::vector<uint8_t>)> done;
      {
        std::lock_guard<std::mutex> lock(shot_mutex);
        if (!shot_callbacks.empty()) {
          done = std::move(shot_callbacks.front());
          shot_callbacks.pop_front();
        }
      }
      if (done && !rgb.empty()) done(front->width, front->height, std::move(rgb));
    }
  }
  if (env_dump) {
    if (front) DumpImage(*front, "front");
    WriteFinalDump();
    if (dump_targets) {
      int n = 0;
      char name[64];
      for (auto& kv : color_targets) {
        if (kv.second->samples > 1) continue;   // a multisampled image cannot be copied to a buffer; its resolved snapshots are dumped below
        std::snprintf(name, sizeof name, "target%d_fmt%u_base%X", n++, static_cast<uint32_t>((kv.first >> 32) & 0xFF), static_cast<uint32_t>(kv.first & 0xFFFFFFFF));
        DumpImage(*kv.second, name);
      }
      for (auto& kv : resolved_color) {
        std::snprintf(name, sizeof name, "resolved_%08X", kv.first);
        DumpImage(*kv.second, name);
      }
      for (auto& kv : resolved_depth) {
        std::snprintf(name, sizeof name, "resolved_depth_%08X", kv.first);
        DumpImage(*kv.second, name);
      }
    }
    ++dump_count;
  }
  cur = nullptr;
  cmd = VK_NULL_HANDLE;
  slot_index = (slot_index + 1) % kFramesInFlight;
}

void Renderer::Impl::ProfMark(const char* fmt, ...) {
  if (!prof || !cur || !prof_pool) return;
  const uint32_t index = static_cast<uint32_t>(cur - slots);
  ProfSlot& p = prof_slots[index];
  if (p.count >= kProfMax) return;
  char label[96];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(label, sizeof label, fmt, ap);
  va_end(ap);
  vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, prof_pool, index * kProfMax + p.count);
  p.labels.emplace_back(label);
  ++p.count;
}

void Renderer::Impl::ProfCollect(uint32_t slot) {
  ProfSlot& p = prof_slots[slot];
  const uint32_t first = slot * kProfMax;
  if (p.count > 1) {
    std::vector<uint64_t> t(p.count);
    if (vkGetQueryPoolResults(vk.device(), prof_pool, first, p.count, t.size() * sizeof(uint64_t), t.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      const double period = vk.properties().limits.timestampPeriod;
      static const double slow_ms = std::getenv("GH2_RENDER_PROFILE_SLOW") ? std::atof(std::getenv("GH2_RENDER_PROFILE_SLOW")) : 14.0;
      static int slow_logged = 0;
      const double total_ms = (t[p.count - 1] - t[0]) * period / 1e6;
      if (total_ms > slow_ms && frame > 1500 && slow_logged < 40) {   // a frame near the 16.7 ms budget: where its GPU time went
        ++slow_logged;
        std::map<std::string, std::pair<uint64_t, uint32_t>> by;
        for (uint32_t i = 0; i + 1 < p.count; ++i) {
          auto& e = by[p.labels[i]];
          e.first += static_cast<uint64_t>((t[i + 1] - t[i]) * period);
          ++e.second;
        }
        std::string text;
        for (auto& kv : by) {
          char m[120];
          std::snprintf(m, sizeof m, " | %s %.2f ms x%u", kv.first.c_str(), kv.second.first / 1e6, kv.second.second);
          text += m;
        }
        std::fprintf(stderr, "RENDER slow GPU frame: %.1f ms%s\n", total_ms, text.c_str());
      }
      std::lock_guard<std::mutex> lock(prof_mutex);
      for (uint32_t i = 0; i + 1 < p.count; ++i) {
        auto& e = prof_acc[p.labels[i]];
        e.first += static_cast<uint64_t>((t[i + 1] - t[i]) * period);
        ++e.second;
      }
      ++prof_frames;
    }
  }
  p.count = 0;
  p.labels.clear();
  vkResetQueryPool(vk.device(), prof_pool, first, kProfMax);
}

namespace {
float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
  float v = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24) : (exponent == 31 ? 65504.f : std::ldexp(1.f + mantissa / 1024.f, static_cast<int>(exponent) - 15));
  return sign ? -v : v;
}
uint8_t To8(float f) { return static_cast<uint8_t>(std::min(255.f, std::max(0.f, f * 255.f + 0.5f))); }
}  // namespace

void Renderer::Impl::DumpImage(const TargetImage& image, const char* name) {
  const VkFormat format = image.image.format;
  uint32_t bpp = 4;
  if (format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R32G32_SFLOAT) bpp = 8;
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(image.width) * image.height * bpp;
  if (!image.image.image) return;
  if (!dump_buffer.buffer || dump_buffer.size < bytes) {
    vkDeviceWaitIdle(vk.device());
    vk.DestroyBuffer(&dump_buffer);
    vk.CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &dump_buffer);
  }
  // a one-shot command buffer on the idle queue: move the image to the copy layout and back
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = slots[0].pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer one = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(vk.device(), &cai, &one);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(one, &bi);
  VkCommandBuffer saved = cmd;
  cmd = one;
  const VkImageLayout layout = image.layout;
  const VkImageAspectFlags copy_aspect = image.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  ImageBarrier(image.image.image, image.aspect, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy region = {};
  region.imageSubresource = {copy_aspect, 0, 0, 1};
  region.imageExtent = {image.width, image.height, 1};
  vkCmdCopyImageToBuffer(one, image.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dump_buffer.buffer, 1, &region);
  ImageBarrier(image.image.image, image.aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
  cmd = saved;
  vkEndCommandBuffer(one);
  VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cbi.commandBuffer = one;
  VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  si.commandBufferInfoCount = 1;
  si.pCommandBufferInfos = &cbi;
  vkQueueSubmit2(vk.queue(), 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(vk.queue());
  vkFreeCommandBuffers(vk.device(), slots[0].pool, 1, &one);

  char path[512];
  std::snprintf(path, sizeof path, "%s_%06llu_%s.ppm", dump_prefix.c_str(), (unsigned long long)frame, name);
  FILE* f = dump_out ? nullptr : std::fopen(path, "wb");
  if (!f && !dump_out) return;
  if (f) std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
  const size_t pixels = static_cast<size_t>(image.width) * image.height;
  std::vector<uint8_t> rgb(pixels * 3);
  const uint8_t* src = dump_buffer.mapped;
  if (image.depth) {   // depth: stretched between its smallest and largest value, which are reported
    float lo = 1e30f, hi = -1e30f;
    for (size_t i = 0; i < pixels; ++i) {
      float v;
      std::memcpy(&v, src + 4 * i, 4);
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    size_t left_one = 0, right_one = 0;
    for (size_t i = 0; i < pixels; ++i) {
      float v;
      std::memcpy(&v, src + 4 * i, 4);
      if (v > 0.99f) ((i % image.width) < image.width / 2 ? left_one : right_one)++;
    }
    {
      float samples[8];
      for (int k = 0; k < 8; ++k) {
        const size_t x = image.width * (2 * k + 1) / 16, y = image.height / 2;
        std::memcpy(&samples[k], src + 4 * (y * image.width + x), 4);
      }
      std::fprintf(stderr, "RENDER depth samples %s (row %u, x = 1/16 .. 15/16): %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f\n", name, image.height / 2, samples[0], samples[1], samples[2], samples[3], samples[4],
                   samples[5], samples[6], samples[7]);
    }
    std::fprintf(stderr, "RENDER depth image %s: min %g max %g, > 0.99: left half %.1f%% right half %.1f%%\n", name, lo, hi, 200.0 * left_one / pixels, 200.0 * right_one / pixels);
    for (size_t i = 0; i < pixels; ++i) {
      float v;
      std::memcpy(&v, src + 4 * i, 4);
      rgb[3 * i] = rgb[3 * i + 1] = rgb[3 * i + 2] = To8(hi > lo ? (v - lo) / (hi - lo) : 0.f);
    }
  }
  for (size_t i = 0; i < pixels && !image.depth; ++i) {
    uint8_t* o = &rgb[3 * i];
    switch (format) {
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
        uint32_t v;
        std::memcpy(&v, src + 4 * i, 4);
        o[0] = static_cast<uint8_t>((v & 0x3FF) >> 2);
        o[1] = static_cast<uint8_t>(((v >> 10) & 0x3FF) >> 2);
        o[2] = static_cast<uint8_t>(((v >> 20) & 0x3FF) >> 2);
        break;
      }
      case VK_FORMAT_R16G16B16A16_SFLOAT:
        for (int c = 0; c < 3; ++c) {
          uint16_t h;
          std::memcpy(&h, src + 8 * i + 2 * c, 2);
          o[c] = To8(HalfToFloat(h));
        }
        break;
      case VK_FORMAT_R16G16_SFLOAT:
        for (int c = 0; c < 2; ++c) {
          uint16_t h;
          std::memcpy(&h, src + 4 * i + 2 * c, 2);
          o[c] = To8(HalfToFloat(h));
        }
        o[2] = 0;
        break;
      case VK_FORMAT_R32_SFLOAT: {
        float v;
        std::memcpy(&v, src + 4 * i, 4);
        o[0] = o[1] = o[2] = To8(v);
        break;
      }
      case VK_FORMAT_R32G32_SFLOAT: {
        float v[2];
        std::memcpy(v, src + 8 * i, 8);
        o[0] = To8(v[0]);
        o[1] = To8(v[1]);
        o[2] = 0;
        break;
      }
      default: {
        static const bool alpha_only = std::getenv("GH2_RENDER_DUMP_ALPHA") != nullptr;   // 8888 images: the alpha channel as grey
        o[0] = alpha_only ? src[4 * i + 3] : src[4 * i];
        o[1] = alpha_only ? src[4 * i + 3] : src[4 * i + 1];
        o[2] = alpha_only ? src[4 * i + 3] : src[4 * i + 2];
        break;
      }
    }
  }
  if (dump_out) {
    dump_out->swap(rgb);
    return;
  }
  std::fwrite(rgb.data(), 1, rgb.size(), f);
  std::fclose(f);
  std::fprintf(stderr, "RENDER dumped %s of frame %llu to %s\n", name, (unsigned long long)frame, path);
}

// ------------------------------------------------------------------------------------------------------------------------------- public interface

Renderer::Renderer() : impl_(new Impl) {}
Renderer::~Renderer() = default;

std::unique_ptr<Renderer> Renderer::Create(const Config& config, std::string* error) {
  (void)error;
  std::unique_ptr<Renderer> renderer(new Renderer());
  renderer->impl_->config = config;
  {   // internal resolution: 16:9 at the requested height (720 = the console's image); the targets, viewports, scissors and resolves of the guest are scaled to it
    const uint32_t h = std::min(4320u, std::max(360u, config.render_height ? config.render_height : 720u));
    renderer->impl_->frame_height = h;
    renderer->impl_->frame_width = (h * 16 + 4) / 9;
    renderer->impl_->opt_height = h;
    std::fprintf(stderr, "RENDER internal resolution %ux%u (scale %.3f)\n", renderer->impl_->frame_width, renderer->impl_->frame_height, h / 720.0);
  }
  if (!config.trace_path.empty() && config.trace_frames) {
    renderer->impl_->trace = std::fopen(config.trace_path.c_str(), "w");
    renderer->impl_->trace_frames_left = config.trace_frames;
    if (const char* skip = std::getenv("GH2_GPU_TRACE_SKIP")) renderer->impl_->trace_skip = static_cast<uint32_t>(std::atoi(skip));
  }
  renderer->impl_->prof = std::getenv("GH2_RENDER_PROFILE") != nullptr;
  renderer->impl_->prof_draws = renderer->impl_->prof && std::atoi(std::getenv("GH2_RENDER_PROFILE")) >= 2;
  renderer->impl_->log_draws = std::getenv("GH2_RENDER_LOG") != nullptr;
  renderer->impl_->load_stats = renderer->impl_->prof || std::getenv("GH2_RENDER_LOAD_STATS") != nullptr;
  if (const char* dbg = std::getenv("GH2_RENDER_DEBUG")) renderer->impl_->debug = static_cast<uint32_t>(std::atoi(dbg));
  if (const char* dump = std::getenv("GH2_RENDER_DUMP"); dump && *dump) {
    renderer->impl_->dump_prefix = dump;
    const char* every = std::getenv("GH2_RENDER_DUMP_EVERY");
    const char* max = std::getenv("GH2_RENDER_DUMP_MAX");
    renderer->impl_->dump_every = every ? static_cast<uint32_t>(std::atoi(every)) : 120;
    renderer->impl_->dump_max = max ? static_cast<uint32_t>(std::atoi(max)) : 20;
    renderer->impl_->dump_targets = std::getenv("GH2_RENDER_DUMP_TARGETS") != nullptr;
    renderer->impl_->dump_final = std::getenv("GH2_RENDER_DUMP_FINAL") != nullptr;
    if (const char* from = std::getenv("GH2_RENDER_DUMP_FROM")) renderer->impl_->dump_from = static_cast<uint32_t>(std::atoi(from));
  }
  return renderer;
}

const RendererStats& Renderer::stats() const { return impl_->stats; }

std::string Renderer::StatsLine() const {
  const RendererStats& s = impl_->stats;
  char buf[768];
  std::snprintf(buf, sizeof buf,
                "render: %llu presented (%llu dropped); draws seen %llu, issued %llu, skipped %llu; resolves %llu; pipelines %llu; textures uploaded %llu; shader misses %llu; ring overflows %llu, ring peak %.1f MB; "
                "vulkan memory %.1f MB; record %.1f ms total; skips noop/vs/scan/prim/idx/mod/tgt/ring/pipe/noinput %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
                (unsigned long long)s.frames_presented.load(), (unsigned long long)s.frames_dropped.load(), (unsigned long long)s.draws_seen.load(), (unsigned long long)s.draws_issued.load(),
                (unsigned long long)s.draws_skipped.load(), (unsigned long long)s.resolves.load(), (unsigned long long)s.pipelines_created.load(), (unsigned long long)s.textures_uploaded.load(),
                (unsigned long long)s.shader_misses.load(), (unsigned long long)s.ring_overflows.load(), s.ring_peak_bytes.load() / 1048576.0, s.gpu_memory_bytes.load() / 1048576.0, s.record_ns.load() / 1e6,
                (unsigned long long)impl_->skip_reasons[0], (unsigned long long)impl_->skip_reasons[1], (unsigned long long)impl_->skip_reasons[2], (unsigned long long)impl_->skip_reasons[3],
                (unsigned long long)impl_->skip_reasons[4], (unsigned long long)impl_->skip_reasons[5], (unsigned long long)impl_->skip_reasons[6], (unsigned long long)impl_->skip_reasons[7],
                (unsigned long long)impl_->skip_reasons[8], (unsigned long long)impl_->skip_reasons[9]);
  std::string line = buf;
  {
    char m[320];
    std::snprintf(m, sizeof m, "\n    IM_LOAD_IMMEDIATE shaders %llu, of which not identified: %llu", (unsigned long long)impl_->immediate_loads.load(), (unsigned long long)impl_->immediate_unknown.load());
    line += m;
    std::snprintf(m, sizeof m, "\n    vertex IM_LOADs %llu, of which the same address with other microcode than its previous load: %llu", (unsigned long long)impl_->vs_loads.load(), (unsigned long long)impl_->vs_loads_changed.load());
    line += m;
    std::snprintf(m, sizeof m, "\n    microcode changed between IM_LOAD and the draw: %llu of %llu shader checks; draws of vertex shaders that export to memory: %llu", (unsigned long long)impl_->microcode_changed.load(),
                  (unsigned long long)impl_->microcode_checked.load(), (unsigned long long)impl_->memexport_draws.load());
    line += m;
    std::snprintf(m, sizeof m, "\n    uploads to the ring: vertices %.0f MB, indices %.0f MB, constants %.0f MB (%llu of %llu constant blocks unchanged)", impl_->up_vertex_bytes.load() / 1048576.0, impl_->up_index_bytes.load() / 1048576.0,
                  impl_->up_const_bytes.load() / 1048576.0, (unsigned long long)impl_->up_const_hits.load(), (unsigned long long)impl_->up_const_total.load());
    line += m;
    std::snprintf(m, sizeof m, "\n    dependent vertex streams uploaded (shader-indexed fetches): %llu, %.0f MB", (unsigned long long)impl_->dependent_fetch_draws.load(), impl_->dependent_fetch_bytes.load() / 1048576.0);
    line += m;
    std::snprintf(m, sizeof m, "\n    clears replayed on the same EDRAM base at another sample count: %llu; clears skipped because the image already held that value: %llu; passes reopened for a pending clear: %llu; snapshot mip chains made: %llu; draws asking for alpha-to-mask: %llu", (unsigned long long)impl_->clear_replays.load(), (unsigned long long)impl_->clears_skipped.load(), (unsigned long long)impl_->clears_reopened.load(), (unsigned long long)impl_->snapshot_mips_made.load(), (unsigned long long)impl_->alpha_to_mask_draws.load());
    line += m;
    char p[320];
    std::snprintf(p, sizeof p, "\n    pipelines: %llu listed at start; %llu built ahead of time (%.0f ms of work), %llu of them used (waited %.1f ms in all); %llu built by a draw (%.0f ms in all, longest %.1f ms)",
                  (unsigned long long)impl_->pipelines_listed_at_start.load(), (unsigned long long)impl_->pipelines_ahead.load(), impl_->ahead_ns.load() / 1e6, (unsigned long long)impl_->ahead_hits.load(),
                  impl_->ahead_wait_ns.load() / 1e6, (unsigned long long)impl_->pipelines_on_demand.load(), impl_->on_demand_ns.load() / 1e6, impl_->on_demand_max_ns.load() / 1e6);
    line += p;
    std::snprintf(p, sizeof p, "\n    textures: %.0f ms re-hashing what the draws use (every frame), %.0f ms decoding %.0f MB of changed ones", impl_->tex_hash_ns.load() / 1e6, impl_->tex_decode_ns.load() / 1e6,
                  impl_->tex_decode_bytes.load() / 1048576.0);
    line += p;
  }
  {
    char t[320];
    std::snprintf(t, sizeof t, "\n    texture heaps used 2D %u / cube %u / samplers %u of %u; fallback texel reads by reason (type/3d+format/range/image/staging/upload/heap/view/resolved): %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu",
                  impl_->heap_next[0], impl_->heap_next[2], impl_->heap_next[3], kHeapDescriptors, (unsigned long long)impl_->tex_fallbacks[0].load(), (unsigned long long)impl_->tex_fallbacks[1].load(),
                  (unsigned long long)impl_->tex_fallbacks[2].load(), (unsigned long long)impl_->tex_fallbacks[3].load(), (unsigned long long)impl_->tex_fallbacks[4].load(), (unsigned long long)impl_->tex_fallbacks[5].load(),
                  (unsigned long long)impl_->tex_fallbacks[6].load(), (unsigned long long)impl_->tex_fallbacks[7].load(), (unsigned long long)impl_->tex_fallbacks[8].load());
    line += t;
  }
  if (impl_->prof) {   // GPU time per kind of segment, per frame, since the previous call
    std::map<std::string, std::pair<uint64_t, uint64_t>> acc;
    uint64_t frames = 0;
    {
      std::lock_guard<std::mutex> lock(impl_->prof_mutex);
      acc.swap(impl_->prof_acc);
      frames = impl_->prof_frames;
      impl_->prof_frames = 0;
    }
    std::vector<std::pair<uint64_t, std::string>> order;
    for (auto& kv : acc) order.emplace_back(kv.second.first, kv.first);
    std::sort(order.rbegin(), order.rend());
    {
      std::lock_guard<std::mutex> lock(impl_->prof_mutex);
      for (auto& kv : impl_->depth_use) {
        char d[120];
        std::snprintf(d, sizeof d, "\n    depth snapshot %08X: resolved %llu times, sampled by draws %llu times", kv.first, (unsigned long long)kv.second.first, (unsigned long long)kv.second.second);
        line += d;
      }
    }
    line += "\n    GPU profile (per frame, " + std::to_string(frames) + " frames):";
    for (size_t i = 0; i < order.size() && i < (impl_->prof_draws ? 22u : 10u); ++i) {
      char m[200];
      std::snprintf(m, sizeof m, "\n      %6.2f ms  %5.1f segments  %s", order[i].first / 1e6 / std::max<uint64_t>(1, frames), static_cast<double>(acc[order[i].second].second) / std::max<uint64_t>(1, frames), order[i].second.c_str());
      line += m;
    }
  }
  // window statistics since the previous call
  uint64_t sh[Impl::kBuckets], ch[Impl::kBuckets];
  for (int i = 0; i < Impl::kBuckets; ++i) {
    sh[i] = impl_->swap_hist[i].exchange(0);
    ch[i] = impl_->cpu_hist[i].exchange(0);
  }
  const uint64_t n = impl_->swap_count.exchange(0), sum = impl_->swap_ns_sum.exchange(0), mx = impl_->swap_ns_max.exchange(0);
  const uint64_t cpu_sum = impl_->cpu_ns_sum.exchange(0), cpu_max = impl_->cpu_ns_max.exchange(0);
  const uint64_t gn = impl_->gpu_frames.exchange(0), gsum = impl_->gpu_ns_sum.exchange(0), gmax = impl_->gpu_ns_max.exchange(0);
  if (n) {
    char w[640];
    std::snprintf(w, sizeof w,
                  "\n    window: %.1f fps (swap interval avg %.2f ms, max %.1f ms); intervals <12/15/18/21/26/34/50/>=50 ms: %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu; renderer CPU/frame avg %.2f ms max %.1f ms "
                  "(<12/15/18/21/26/34/50/>=50: %llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu); GPU/frame avg %.2f ms max %.1f ms",
                  1e9 * n / sum, sum / 1e6 / n, mx / 1e6, (unsigned long long)sh[0], (unsigned long long)sh[1], (unsigned long long)sh[2], (unsigned long long)sh[3], (unsigned long long)sh[4],
                  (unsigned long long)sh[5], (unsigned long long)sh[6], (unsigned long long)sh[7], cpu_sum / 1e6 / n, cpu_max / 1e6, (unsigned long long)ch[0], (unsigned long long)ch[1],
                  (unsigned long long)ch[2], (unsigned long long)ch[3], (unsigned long long)ch[4], (unsigned long long)ch[5], (unsigned long long)ch[6], (unsigned long long)ch[7],
                  gn ? gsum / 1e6 / gn : 0.0, gmax / 1e6);
    line += w;
    uint64_t sec[8];
    for (int i = 0; i < 8; ++i) sec[i] = impl_->section_ns[i].exchange(0);
    std::snprintf(w, sizeof w, "\n    draw cost per frame (ms): shaders %.2f, indices %.2f, modules+targets+vertex input %.2f, textures %.2f, uploads %.2f, pipeline lookup %.2f, record %.2f , vertex streams %.2f (counted apart from uploads)",
                  sec[0] / 1e6 / n, sec[1] / 1e6 / n, sec[2] / 1e6 / n, sec[3] / 1e6 / n, sec[4] / 1e6 / n, sec[5] / 1e6 / n, sec[6] / 1e6 / n, sec[7] / 1e6 / n);
    line += w;
  }
  return line;
}

uint64_t MicrocodeShapeHash(const uint32_t* words, size_t count) {
  const gh2gfx::d3d::FetchUsage usage = gh2gfx::d3d::ScanFetches(words, count);
  std::vector<bool> skip(count, false);
  for (const gh2gfx::d3d::VertexFetch& vf : usage.vertex_fetches)
    for (size_t k = 0; k < 3; ++k)
      if (static_cast<size_t>(vf.instruction) * 3 + k < count) skip[vf.instruction * 3 + k] = true;
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < count; ++i)
    if (!skip[i]) h = (h ^ words[i]) * 1099511628211ull;
  return h ^ count;
}

void Renderer::RegisterShader(uint32_t microcode_physical, uint64_t container_hash, bool vertex, std::vector<VertexElementInfo> elements, uint64_t shape_hash) {
  auto info = std::make_shared<ShaderInfo>();
  info->hash = container_hash;
  info->vertex = vertex;
  info->elements = std::move(elements);
  std::lock_guard<std::mutex> lock(impl_->shader_mutex);
  if (shape_hash) impl_->shaders_by_shape[shape_hash] = info;
  impl_->shaders_by_microcode[microcode_physical] = std::move(info);
}

void Renderer::NoteCpuWrite(uint32_t, uint32_t) {}

void Renderer::OnShaderLoad(uint32_t type, uint32_t physical, uint32_t size_dwords) {
  Impl& s = *impl_;
  std::shared_ptr<const ShaderInfo> info;
  {
    std::lock_guard<std::mutex> lock(s.shader_mutex);
    auto it = s.shaders_by_microcode.find(physical);
    if (it != s.shaders_by_microcode.end()) info = it->second;
  }
  ActiveShader& a = type == 0 ? s.active_vertex : s.active_pixel;
  a.info = std::move(info);
  a.address = physical;
  a.size_dwords = size_dwords;
  a.immediate = false;
  a.scanned = false;
  a.at_load.clear();
  if (a.info && size_dwords && size_dwords <= 4096 && physical + size_dwords * 4ull <= 0x20000000) {
    const uint8_t* source = s.config.memory.Physical(physical);
    a.at_load.resize(size_dwords);
    for (uint32_t i = 0; i < size_dwords; ++i) {
      uint32_t v;
      std::memcpy(&v, source + 4 * i, 4);
      a.at_load[i] = __builtin_bswap32(v);
    }
  }
  if (type == 0 && s.load_stats && !a.at_load.empty()) {   // statistics (GH2_RENDER_LOAD_STATS=1 or the profiler; the hash costs a few percent of this thread): how often the same vertex microcode address is loaded again with other contents
    static std::unordered_map<uint32_t, uint64_t> last_hash;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t w : a.at_load) h = (h ^ w) * 1099511628211ull;
    auto it = last_hash.find(physical);
    ++s.vs_loads;
    if (it != last_hash.end() && it->second != h) ++s.vs_loads_changed;
    last_hash[physical] = h;
  }
  if (s.Tracing()) s.Tracef("L type=%u addr=%08X size=%u hash=%016llX\n", type, physical, size_dwords, (unsigned long long)(a.info ? a.info->hash : 0));
}

void Renderer::OnShaderLoadImmediate(uint32_t type, const uint32_t* words, uint32_t size_dwords) {
  Impl& s = *impl_;
  std::shared_ptr<const ShaderInfo> info;
  if (size_dwords && size_dwords <= 4096) {
    const uint64_t shape = MicrocodeShapeHash(words, size_dwords);
    std::lock_guard<std::mutex> lock(s.shader_mutex);
    auto it = s.shaders_by_shape.find(shape);
    if (it != s.shaders_by_shape.end()) info = it->second;
  }
  if (!info && size_dwords) {   // the library's own shaders: known by the hash of their microcode (see builtin/)
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < size_dwords; ++i) h = (h ^ words[i]) * 1099511628211ull;
    auto make = [&](uint64_t id, bool vertex, std::vector<VertexElementInfo> elements) {
      auto built = std::make_shared<ShaderInfo>();
      built->hash = id;
      built->vertex = vertex;
      built->elements = std::move(elements);
      return built;
    };
    if (type == 0 && h == 0x1443297898A19A76ull) info = make(kBuiltinClearVs, true, {{3, 0, 0, 1, 0xA88}, {4, 10, 0, 0, 0x688}});   // position (3 floats), colour (4 floats)
    else if (type == 0 && h == 0x2633A3A81DFFE897ull) info = make(kBuiltinRectVs, true, {{2, 0, 0, 0, 0xB48}});                       // position (2 floats)
    else if (type == 1 && h == 0xDB8EA0EB7FC160B9ull) info = make(kBuiltinPassPs, false, {});                                          // colour = interpolator 0
  }
  ActiveShader& a = type == 0 ? s.active_vertex : s.active_pixel;
  a.info = std::move(info);
  a.address = 0;
  a.size_dwords = size_dwords;
  a.immediate = true;
  a.microcode.assign(words, words + size_dwords);
  a.at_load = a.microcode;
  a.scanned = false;
  ++s.immediate_loads;
  if (!a.info && std::getenv("GH2_RENDER_DUMP_BUILTIN")) {   // debugging: the library's own shaders (no container): each distinct microcode once, as words
    static std::unordered_map<uint64_t, bool> dumped;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < size_dwords; ++i) h = (h ^ words[i]) * 1099511628211ull;
    if (!dumped[h]) {
      dumped[h] = true;
      char path[256];
      std::snprintf(path, sizeof path, "%s_t%u_%016llX.bin", std::getenv("GH2_RENDER_DUMP_BUILTIN"), type, (unsigned long long)h);
      if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(words, 4, size_dwords, f);
        std::fclose(f);
      }
    }
  }
  if (!a.info) {
    ++s.immediate_unknown;
    static int shown = 0;
    if (shown++ < 12) {   // what a microcode that matches no registered shader looks like
      const gh2gfx::d3d::FetchUsage u = gh2gfx::d3d::ScanFetches(words, size_dwords);
      std::fprintf(stderr, "RENDER unidentified IM_LOAD_IMMEDIATE: type %u, %u dwords, %zu vertex fetches, %u texture slots mask %08X, first dwords %08X %08X %08X %08X, shape %016llX\n", type, size_dwords,
                   u.vertex_fetches.size(), __builtin_popcount(u.texture_slots), u.texture_slots, words[0], size_dwords > 1 ? words[1] : 0, size_dwords > 2 ? words[2] : 0, size_dwords > 3 ? words[3] : 0,
                   (unsigned long long)MicrocodeShapeHash(words, size_dwords));
    }
  }
  if (s.Tracing()) s.Tracef("LI type=%u size=%u hash=%016llX\n", type, size_dwords, (unsigned long long)(a.info ? a.info->hash : 0));
}

void Renderer::OnDraw(const uint32_t* regs, const gh2gpu::DrawParams& d) {
  Impl& s = *impl_;
  namespace r = xenos::reg;
  ++s.stats.draws_seen;
  ++s.draws_this_frame;
  const uint32_t mode = regs[r::RB_MODECONTROL] & 7;
  if (s.Tracing()) {
    s.Tracef("%s prim=%u n=%u src=%u i32=%d dma=%08X sz=%u | vs=%016llX ps=%016llX | surf=%08X col=%08X d=%08X c1=%08X mode=%u | scissor=%08X-%08X off=%08X | depth=%08X blend0=%08X cc=%08X mask=%08X sc=%08X vte=%08X | "
             "vp=%g,%g,%g,%g | copy=%08X dst=%08X pitch=%08X info=%08X clear=%08X/%08X\n",
             mode == 6 ? "R" : "D", d.prim_type, d.num_indices, d.source_select, d.index32, d.dma_base, d.dma_size_bytes, (unsigned long long)(s.active_vertex.info ? s.active_vertex.info->hash : 0),
             (unsigned long long)(s.active_pixel.info ? s.active_pixel.info->hash : 0), regs[r::RB_SURFACE_INFO], regs[r::RB_COLOR_INFO], regs[r::RB_DEPTH_INFO], regs[r::RB_COLOR1_INFO], mode,
             regs[r::PA_SC_WINDOW_SCISSOR_TL], regs[r::PA_SC_WINDOW_SCISSOR_BR], regs[r::PA_SC_WINDOW_OFFSET], regs[r::RB_DEPTHCONTROL], regs[r::RB_BLENDCONTROL0], regs[r::RB_COLORCONTROL],
             regs[r::RB_COLOR_MASK], regs[r::PA_SU_SC_MODE_CNTL], regs[r::PA_CL_VTE_CNTL], xenos::AsFloat(regs[r::PA_CL_VPORT_XSCALE]), xenos::AsFloat(regs[r::PA_CL_VPORT_XOFFSET]),
             xenos::AsFloat(regs[r::PA_CL_VPORT_YSCALE]), xenos::AsFloat(regs[r::PA_CL_VPORT_YOFFSET]), regs[r::RB_COPY_CONTROL], regs[r::RB_COPY_DEST_BASE], regs[r::RB_COPY_DEST_PITCH],
             regs[r::RB_COPY_DEST_INFO], regs[r::RB_COLOR_CLEAR], regs[r::RB_DEPTH_CLEAR]);
    for (uint32_t i = 0; i < 16; ++i) {
      const uint32_t* f = &regs[r::SHADER_CONSTANT_FETCH + 6 * i];
      if ((f[0] & 3) == 2 && f[1]) s.Tracef("   t%u: %08X %08X %08X %08X %08X %08X\n", i, f[0], f[1], f[2], f[3], f[4], f[5]);
    }
  }
  const uint64_t t0 = NowNs();
  if (mode == 6) {   // EDRAM copy mode: the resolve (render target -> texture memory)
    ++s.stats.resolves;
    ++s.resolves_this_frame;
    if (s.BeginFrame()) s.Resolve(regs);
  } else if (mode == 4 || mode == 5 || mode == 1) {
    if (s.BeginFrame()) s.Draw(regs, d);
  }
  const uint64_t spent = NowNs() - t0;
  s.stats.record_ns += spent;
  s.frame_cpu_ns += spent;
}

void Renderer::OnSwap(const uint32_t* regs, uint32_t front, uint32_t width, uint32_t height) {
  Impl& s = *impl_;
  (void)regs;
  if (s.Tracing())
    s.Tracef("S frame=%llu front=%08X %ux%u draws=%llu resolves=%llu\n", (unsigned long long)s.frame, front, width, height, (unsigned long long)s.draws_this_frame, (unsigned long long)s.resolves_this_frame);
  if (s.trace) {
    if (s.trace_skip) --s.trace_skip;
    else if (s.trace_frames_left && --s.trace_frames_left == 0) {
      std::fclose(s.trace);
      s.trace = nullptr;
      std::fprintf(stderr, "RENDER trace complete\n");
    }
  }
  ++s.frame;
  s.draws_this_frame = s.resolves_this_frame = s.drawn_this_frame = 0;
  s.draw_in_frame = 0;
  const uint64_t t0 = NowNs();
  if (!s.BeginFrame()) {
    ++s.stats.frames_dropped;
    return;
  }
  s.SubmitFrame(true, s.FindResolved(front));
  if (s.config.on_present) {
    const VkExtent2D ext = s.vk.swapchain_extent();
    s.config.on_present(ext.width, ext.height);
  }
  const uint64_t t1 = NowNs();
  s.stats.record_ns += t1 - t0;
  s.frame_cpu_ns += t1 - t0;
  // frame statistics: the time between swaps (what the player sees as frame rate) and the renderer's own CPU time per frame
  if (s.last_swap_ns) {
    const uint64_t interval = t1 - s.last_swap_ns;
    ++s.swap_hist[Impl::Bucket(interval)];
    s.swap_ns_sum += interval;
    ++s.swap_count;
    if (interval > s.swap_ns_max) s.swap_ns_max = interval;
  }
  {   // a frame that took long (renderer CPU over GH2_RENDER_HITCH_MS, default 20 ms, or a swap interval over 42 ms): where the time went
    static const double hitch_ms = std::getenv("GH2_RENDER_HITCH_MS") ? std::atof(std::getenv("GH2_RENDER_HITCH_MS")) : 20.0;   // renderer CPU; a swap interval counts from 2.5 times the 60 Hz frame (the game boots at 30 fps by itself)
    const double interval_ms = s.last_swap_ns ? (t1 - s.last_swap_ns) / 1e6 : 0.0, cpu_ms = s.frame_cpu_ns / 1e6;
    uint64_t delta[8];
    for (int i = 0; i < 8; ++i) {
      const uint64_t now = s.section_ns[i].load();
      delta[i] = now >= s.section_prev[i] ? now - s.section_prev[i] : now;   // the periodic statistics line resets the counters
      s.section_prev[i] = now;
    }
    const uint64_t textures = s.stats.textures_uploaded.load(), pipelines = s.stats.pipelines_created.load();
    if ((interval_ms > 42.0 || cpu_ms > hitch_ms) && s.hitches_logged < 200) {
      ++s.hitches_logged;
      std::fprintf(stderr, "RENDER hitch: frame %llu interval %.1f ms, renderer CPU %.1f ms (shaders %.1f, indices %.1f, modules+targets %.1f, textures %.1f, uploads %.1f, pipeline lookup %.1f, record %.1f); %llu textures uploaded, %llu pipelines created\n",
                   (unsigned long long)s.frame, interval_ms, cpu_ms, delta[0] / 1e6, delta[1] / 1e6, delta[2] / 1e6, delta[3] / 1e6, delta[4] / 1e6, delta[5] / 1e6, delta[6] / 1e6,
                   (unsigned long long)(textures - s.hitch_textures_prev), (unsigned long long)(pipelines - s.hitch_pipelines_prev));
    }
    s.hitch_textures_prev = textures;
    s.hitch_pipelines_prev = pipelines;
  }
  s.last_swap_ns = t1;
  ++s.cpu_hist[Impl::Bucket(s.frame_cpu_ns)];
  s.cpu_ns_sum += s.frame_cpu_ns;
  if (s.frame_cpu_ns > s.cpu_ns_max) s.cpu_ns_max = s.frame_cpu_ns;
  s.frame_cpu_ns = 0;
}

void Renderer::OnIdle() {}

}  // namespace gh2render
