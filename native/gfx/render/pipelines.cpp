// Pipelines: the lookup a draw does, the pipelines built ahead of time, and the two files that make that possible.
//
// Creating a graphics pipeline costs 4-20 ms (the driver compiles the shaders for this exact combination of state), and a song asks for ~350 of them, many of them for the first time in the
// middle of a frame. Two things remove that cost from the frames:
//  - pipeline_cache.bin: the driver's own cache blob (VkPipelineCache), loaded at start and saved while the game runs. A pipeline the driver has compiled before costs a fraction of that.
//  - pipelines.list: the description of every pipeline the game has asked for, in order of first use. At start a low-priority thread builds them all (with the driver's cache, usually quickly)
//    while the game is still in its intro; a draw that needs one finds it done (or waits for the few milliseconds that are left when the thread is just building it).
// A pipeline nobody listed yet (only the first time a machine sees that scene) is compiled by a pool of threads, side by side with the others the scene asks for, and joins
// the list; the draw waits for it within a small budget per frame and is otherwise skipped until it is ready, so a new venue costs a missing object for a frame or two
// instead of frames of 0.1-0.8 s. GH2_PIPELINE_ASYNC=0 builds it in the draw, as before. Same code on Linux and Windows.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <gh2os/os.h>

#include "impl.h"

namespace gh2render {

namespace {

constexpr char kListMagic[8] = {'G', 'H', '2', 'P', 'L', 'S', 'T', '2'};   // 2: the record has the sample count
constexpr uint32_t kSaveIntervalSeconds = 15;

// the fixed part of a serialized PipelineDesc; the binding and attribute descriptions follow it as they are in Vulkan
struct Wire {
  uint64_t vs_hash, ps_hash;
  uint32_t vs_spec, ps_spec, samples, rect, restart, topology, color_format, color_mask, blend, depth_control, mode_control, binding_count, attribute_count;
};

std::string Serialize(const PipelineDesc& d) {
  Wire w = {d.vs_hash,  d.ps_hash,   d.vs_spec,       d.ps_spec,       d.samples,       d.rect ? 1u : 0u,
            d.primitive_restart ? 1u : 0u, static_cast<uint32_t>(d.topology), static_cast<uint32_t>(d.color_format), d.color_mask, d.blend,
            d.depth_control, d.mode_control, static_cast<uint32_t>(d.bindings.size()), static_cast<uint32_t>(d.attributes.size())};
  std::string out(reinterpret_cast<const char*>(&w), sizeof w);
  out.append(reinterpret_cast<const char*>(d.bindings.data()), d.bindings.size() * sizeof(VkVertexInputBindingDescription));
  out.append(reinterpret_cast<const char*>(d.attributes.data()), d.attributes.size() * sizeof(VkVertexInputAttributeDescription));
  return out;
}

// One record starting at p (n bytes left); returns the number of bytes it takes, 0 when it is damaged.
size_t Deserialize(const uint8_t* p, size_t n, PipelineDesc* d) {
  Wire w;
  if (n < sizeof w) return 0;
  std::memcpy(&w, p, sizeof w);
  if (w.binding_count > 16 || w.attribute_count > 32) return 0;
  const size_t bytes = sizeof w + w.binding_count * sizeof(VkVertexInputBindingDescription) + w.attribute_count * sizeof(VkVertexInputAttributeDescription);
  if (n < bytes) return 0;
  d->vs_hash = w.vs_hash;
  d->ps_hash = w.ps_hash;
  d->vs_spec = w.vs_spec;
  d->ps_spec = w.ps_spec;
  d->samples = (w.samples == 2 || w.samples == 4) ? w.samples : 1;
  d->rect = w.rect != 0;
  d->primitive_restart = w.restart != 0;
  d->topology = static_cast<VkPrimitiveTopology>(w.topology);
  d->color_format = static_cast<VkFormat>(w.color_format);
  d->color_mask = w.color_mask;
  d->blend = w.blend;
  d->depth_control = w.depth_control;
  d->mode_control = w.mode_control;
  d->bindings.resize(w.binding_count);
  d->attributes.resize(w.attribute_count);
  std::memcpy(d->bindings.data(), p + sizeof w, w.binding_count * sizeof(VkVertexInputBindingDescription));
  std::memcpy(d->attributes.data(), p + sizeof w + w.binding_count * sizeof(VkVertexInputBindingDescription), w.attribute_count * sizeof(VkVertexInputAttributeDescription));
  return bytes;
}

bool Listable(const PipelineDesc& d) { return (d.vs_hash >> 48) != 0xDEB0 && (d.ps_hash >> 48) != 0xDEB0; }   // not a debugging stand-in shader

std::string DefaultDirectory() {
  if (const char* e = std::getenv("GH2_PIPELINE_CACHE_DIR"); e && *e) return e;
  return gh2os::CacheDir();   // $XDG_CACHE_HOME/gh2 or ~/.cache/gh2 | %LOCALAPPDATA%\gh2\cache
}

}  // namespace

std::vector<uint8_t> Renderer::Impl::LoadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void Renderer::Impl::SaveFile(const std::string& path, const void* data, size_t size) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!f) return;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);   // atomic: a crash leaves the previous file
}

std::string Renderer::Impl::PipelineKey(const PipelineDesc& desc) const { return Serialize(desc); }

void Renderer::Impl::OpenPipelineCache() {
  const char* off = std::getenv("GH2_PIPELINE_CACHE");   // "off" / "0": neither read nor written (to measure what the cache is worth)
  const bool disabled = off && (!std::strcmp(off, "off") || !std::strcmp(off, "0"));
  pipeline_cache_dir = disabled ? std::string() : (config.pipeline_cache_dir.empty() ? DefaultDirectory() : config.pipeline_cache_dir.string());
  std::vector<uint8_t> blob;
  if (!pipeline_cache_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(pipeline_cache_dir, ec);
    blob = LoadFile(pipeline_cache_dir + "/pipeline_cache.bin");
    const std::vector<uint8_t> list = LoadFile(pipeline_cache_dir + "/pipelines.list");
    if (list.size() >= 16 && !std::memcmp(list.data(), kListMagic, 8)) {
      uint32_t count = 0;
      std::memcpy(&count, list.data() + 8, 4);
      size_t at = 16;
      PipelineDesc scratch;
      for (uint32_t i = 0; i < count && at < list.size(); ++i) {
        const size_t used = Deserialize(list.data() + at, list.size() - at, &scratch);
        if (!used) break;
        std::string record(reinterpret_cast<const char*>(list.data() + at), used);
        if (pipeline_listed.emplace(record, true).second) pipeline_list.push_back(std::move(record));
        at += used;
      }
    }
    pipelines_listed_at_start = pipeline_list.size();
  }
  VkPipelineCacheCreateInfo pcci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  pcci.initialDataSize = blob.size();
  pcci.pInitialData = blob.empty() ? nullptr : blob.data();
  if (vkCreatePipelineCache(vk.device(), &pcci, nullptr, &pipeline_cache) != VK_SUCCESS && !blob.empty()) {   // a blob the driver refuses: start empty
    pcci.initialDataSize = 0;
    pcci.pInitialData = nullptr;
    vkCreatePipelineCache(vk.device(), &pcci, nullptr, &pipeline_cache);
  }
  std::fprintf(stderr, "RENDER pipeline cache: %s (driver blob %zu KB, %zu pipelines listed)\n", pipeline_cache_dir.empty() ? "off" : pipeline_cache_dir.c_str(), blob.size() / 1024, pipeline_list.size());
}

void Renderer::Impl::SavePipelineFiles() {
  if (pipeline_cache_dir.empty() || !pipeline_cache) return;
  size_t size = 0;
  if (vkGetPipelineCacheData(vk.device(), pipeline_cache, &size, nullptr) == VK_SUCCESS && size) {
    std::vector<uint8_t> blob(size);
    if (vkGetPipelineCacheData(vk.device(), pipeline_cache, &size, blob.data()) == VK_SUCCESS) SaveFile(pipeline_cache_dir + "/pipeline_cache.bin", blob.data(), size);
  }
  std::string list;
  {
    std::lock_guard<std::mutex> lock(list_mutex);
    list.assign(kListMagic, 8);
    const uint32_t count = static_cast<uint32_t>(pipeline_list.size()), zero = 0;
    list.append(reinterpret_cast<const char*>(&count), 4);
    list.append(reinterpret_cast<const char*>(&zero), 4);
    for (const std::string& r : pipeline_list) list += r;
  }
  SaveFile(pipeline_cache_dir + "/pipelines.list", list.data(), list.size());
}

void Renderer::Impl::NotePipelineUsed(const PipelineDesc& desc, const std::string& key) {
  if (pipeline_cache_dir.empty() || debug != 0 || !Listable(desc)) return;
  std::lock_guard<std::mutex> lock(list_mutex);
  if (!pipeline_listed.emplace(key, true).second) return;
  pipeline_list.push_back(key);
  list_dirty = true;
}

VkPipeline Renderer::Impl::GetPipeline(const PipelineDesc& desc) {
  const std::string key = Serialize(desc);
  auto found = pipelines.find(key);
  if (found != pipelines.end()) return found->second;
  if (!async_pipelines || compile_workers.empty()) return GetPipelineBlocking(desc, key);

  // being built (by the thread of the listed pipelines or by the pool), or new: then the pool builds it
  std::shared_ptr<AheadEntry> entry;
  bool queue_it = false;
  {
    std::lock_guard<std::mutex> lock(ahead_mutex);
    auto it = ahead.find(key);
    if (it != ahead.end()) {
      entry = it->second;
    } else {
      entry = std::make_shared<AheadEntry>();
      entry->by_pool = true;
      ahead.emplace(key, entry);   // claimed: the thread of the listed pipelines skips it
      queue_it = true;
    }
  }
  if (queue_it) {
    {
      std::lock_guard<std::mutex> lock(compile_mutex);
      compile_queue.push_back(CompileJob{key, desc, entry});
      const uint64_t depth = compile_queue.size();
      uint64_t seen = compile_queue_max.load();
      while (depth > seen && !compile_queue_max.compare_exchange_weak(seen, depth)) {
      }
    }
    compile_cv.notify_one();
  }

  // wait for it within what is left of this frame's budget
  if (frame != budget_frame) {
    budget_frame = frame;
    budget_used_ns = 0;
  }
  const uint64_t left = budget_used_ns < wait_budget_ns ? wait_budget_ns - budget_used_ns : 0;
  const auto t0 = std::chrono::steady_clock::now();
  VkPipeline pipeline = VK_NULL_HANDLE;
  bool ready;
  {
    std::unique_lock<std::mutex> lock(ahead_mutex);
    ready = ahead_cv.wait_for(lock, std::chrono::nanoseconds(left), [&] { return entry->done; });
    if (ready) {
      pipeline = entry->pipeline;
      ahead.erase(key);
    }
  }
  const uint64_t waited = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  budget_used_ns += waited;
  ahead_wait_ns += waited;
  if (!ready) {   // skipped this frame; drawn from the frame its pipeline is ready in
    ++draws_waiting_pipeline;
    return VK_NULL_HANDLE;
  }
  if (entry->by_pool) {
    ++pipelines_async;
    NotePipelineUsed(desc, key);
  } else {
    ++ahead_hits;
  }
  pipelines[key] = pipeline;
  return pipeline;
}

// The pool: builds the pipelines the draws hand it, in the order they asked for them.
void Renderer::Impl::CompileWorkerMain() {
  for (;;) {
    CompileJob job;
    {
      std::unique_lock<std::mutex> lock(compile_mutex);
      compile_cv.wait(lock, [&] { return compile_stop || !compile_queue.empty(); });
      if (compile_stop) return;
      job = std::move(compile_queue.front());
      compile_queue.pop_front();
    }
    const auto t0 = std::chrono::steady_clock::now();
    VkPipeline pipeline = BuildPipeline(job.desc);   // (vkCreateGraphicsPipelines and the pipeline cache are thread-safe; the shader modules exist already)
    const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    ++pipelines_on_demand;
    on_demand_ns += ns;
    uint64_t seen = on_demand_max_ns.load();
    while (ns > seen && !on_demand_max_ns.compare_exchange_weak(seen, ns)) {
    }
    {
      std::lock_guard<std::mutex> lock(ahead_mutex);
      job.entry->pipeline = pipeline;
      job.entry->done = true;
    }
    ahead_cv.notify_all();
  }
}

void Renderer::Impl::StartCompilePool() {
  if (!compile_workers.empty()) return;
  if (const char* e = std::getenv("GH2_PIPELINE_ASYNC"); e && e[0] == '0') async_pipelines = false;
  if (const char* e = std::getenv("GH2_PIPELINE_WAIT_MS"); e && *e) wait_budget_ns = static_cast<uint64_t>(std::max(0.0, std::atof(e)) * 1e6);
  if (!async_pipelines) {
    std::fprintf(stderr, "RENDER pipelines: built by the draw that needs them (GH2_PIPELINE_ASYNC=0)\n");
    return;
  }
  // a few threads: the game keeps its own (a 4-core CPU gets 2); the drivers compile one pipeline per thread
  const unsigned cores = std::max(2u, std::thread::hardware_concurrency());
  const unsigned n = std::clamp(cores / 3, 2u, 6u);
  compile_stop = false;
  for (unsigned i = 0; i < n; ++i)
    compile_workers.emplace_back([this, i] {
      gh2os::SetCurrentThreadName(("gh2-pipeline" + std::to_string(i)).c_str());
      CompileWorkerMain();
    });
  std::fprintf(stderr, "RENDER pipelines: new ones are compiled by %u threads; the draws of a frame wait %.1f ms for them in all, then go without them until they are ready\n", n,
               wait_budget_ns / 1e6);
}

void Renderer::Impl::StopCompilePool() {
  {
    std::lock_guard<std::mutex> lock(compile_mutex);
    compile_stop = true;
    compile_queue.clear();   // (their entries stay in `ahead`, never done: nothing waits for them any more)
  }
  compile_cv.notify_all();
  for (std::thread& t : compile_workers) t.join();
  compile_workers.clear();
}

// GH2_PIPELINE_ASYNC=0 (and no pool): the draw builds a new pipeline itself and waits for one the thread of the listed pipelines is building.
VkPipeline Renderer::Impl::GetPipelineBlocking(const PipelineDesc& desc, const std::string& key) {
  std::shared_ptr<AheadEntry> entry;
  bool mine = false;
  {
    std::lock_guard<std::mutex> lock(ahead_mutex);
    auto it = ahead.find(key);
    if (it != ahead.end()) {
      entry = it->second;
    } else {
      entry = std::make_shared<AheadEntry>();
      ahead.emplace(key, entry);   // claimed: the pipeline thread skips it
      mine = true;
    }
  }
  if (!mine) {   // the pipeline thread has built it, or is just building it
    const auto t0 = std::chrono::steady_clock::now();
    VkPipeline pipeline;
    {
      std::unique_lock<std::mutex> lock(ahead_mutex);
      ahead_cv.wait(lock, [&] { return entry->done; });
      pipeline = entry->pipeline;
      ahead.erase(key);
    }
    const uint64_t waited = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    ahead_wait_ns += waited;
    if (waited > 4000000) std::fprintf(stderr, "RENDER slow: waited %.1f ms for a pipeline the background thread was building\n", waited / 1e6);
    ++ahead_hits;
    pipelines[key] = pipeline;
    return pipeline;
  }

  const auto t0 = std::chrono::steady_clock::now();
  VkPipeline pipeline = BuildPipeline(desc);
  const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
  if (ns > 4000000) std::fprintf(stderr, "RENDER slow: pipeline creation %.1f ms (%016llX / %016llX)\n", ns / 1e6, (unsigned long long)desc.vs_hash, (unsigned long long)desc.ps_hash);
  ++pipelines_on_demand;
  on_demand_ns += ns;
  uint64_t seen = on_demand_max_ns.load();
  while (ns > seen && !on_demand_max_ns.compare_exchange_weak(seen, ns)) {
  }
  {
    std::lock_guard<std::mutex> lock(ahead_mutex);
    ahead.erase(key);
  }
  pipelines[key] = pipeline;
  NotePipelineUsed(desc, key);
  return pipeline;
}

void Renderer::Impl::StartPipelineThread() {
  if (pipeline_thread.joinable()) return;
  StartCompilePool();
  thread_stop = false;
  pipeline_thread = std::thread([this] { PipelineThreadMain(); });
}

void Renderer::Impl::StopPipelineThread() {
  StopCompilePool();   // before the device objects go (DestroyDeviceObjects)
  if (!pipeline_thread.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(thread_mutex);
    thread_stop = true;
  }
  thread_cv.notify_all();
  pipeline_thread.join();
  SavePipelineFiles();
}

void Renderer::Impl::PipelineThreadMain() {
  gh2os::LowerCurrentThreadPriority();   // the game's own threads come first
  std::vector<std::string> todo;
  {
    std::lock_guard<std::mutex> lock(list_mutex);
    todo = pipeline_list;
  }
  ahead_active = !todo.empty();
  const auto begin = std::chrono::steady_clock::now();
  for (const std::string& record : todo) {
    if (thread_stop) break;
    PipelineDesc desc;
    if (!Deserialize(reinterpret_cast<const uint8_t*>(record.data()), record.size(), &desc)) continue;
    desc.vs = GetShaderModule(desc.vs_hash, gh2gfx::ShaderStage::kVertex, nullptr);
    desc.ps = desc.ps_hash ? GetShaderModule(desc.ps_hash, gh2gfx::ShaderStage::kPixel, nullptr) : VK_NULL_HANDLE;
    if (!desc.vs || (desc.ps_hash && !desc.ps)) continue;   // a shader this cache does not have
    std::shared_ptr<AheadEntry> entry;
    {
      std::lock_guard<std::mutex> lock(ahead_mutex);
      if (ahead.count(record)) continue;   // a draw already took it
      entry = std::make_shared<AheadEntry>();
      ahead.emplace(record, entry);
    }
    const auto t0 = std::chrono::steady_clock::now();
    VkPipeline pipeline = BuildPipeline(desc);
    ahead_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    ++pipelines_ahead;
    {
      std::lock_guard<std::mutex> lock(ahead_mutex);
      entry->pipeline = pipeline;
      entry->done = true;
    }
    ahead_cv.notify_all();
  }
  if (!todo.empty())
    std::fprintf(stderr, "RENDER pipelines ahead of time: %llu of %zu listed, %.0f ms of work in %.1f s\n", (unsigned long long)pipelines_ahead.load(), todo.size(), ahead_ns.load() / 1e6,
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
  ahead_active = false;
  uint64_t saved_at = stats.pipelines_created.load();
  for (;;) {
    std::unique_lock<std::mutex> lock(thread_mutex);
    if (thread_cv.wait_for(lock, std::chrono::seconds(kSaveIntervalSeconds), [&] { return thread_stop.load(); })) return;   // the final save is StopPipelineThread's
    lock.unlock();
    if (stats.pipelines_created.load() != saved_at || list_dirty) {
      saved_at = stats.pipelines_created.load();
      list_dirty = false;
      SavePipelineFiles();
    }
  }
}

}  // namespace gh2render
