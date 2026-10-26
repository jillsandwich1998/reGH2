#include "observer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "graphics_state.h"
#include "shader_cache.h"

namespace gh2gfx::d3d {

namespace {

inline uint32_t Be32(const uint8_t* base, uint32_t guest_address) {
  uint32_t v;
  std::memcpy(&v, base + guest_address, sizeof(v));
  return __builtin_bswap32(v);
}

// The library's virtual -> physical conversion for the address it puts into IM_LOAD (read from its code, sub_823D4CC8): bits 28..1 of the address
// plus 0x1000 for the 4 KB-page view (0xE0000000 and up).
uint32_t PhysicalOf(uint32_t virt) { return (virt & 0x1FFFFFFEu) + (((virt >> 20) + 0x200u) & 0x1000u); }

constexpr uint32_t kPm4ImLoad = 0x27;

struct Original {
  uint64_t hash = 0;
  Stage stage = Stage::kPixel;
  bool in_cache = false;
  uint32_t physical_offset = 0;     // of the microcode inside the container's physical part
  std::vector<uint32_t> ucode;      // the container's microcode, host-order words of the big-endian stream
};

struct Object {
  std::shared_ptr<const Original> original;
  uint32_t container = 0;
};

struct StageStats {
  uint64_t created = 0;              // shader objects created
  uint64_t bound_draws = 0;          // draw-state commits with an object of this stage bound
  uint64_t registered_draws = 0;     // ... whose object was created through the hooked functions
  uint64_t cache_hit_draws = 0;      // ... and whose container is in the shader cache
  uint64_t im_loads = 0;             // IM_LOAD packets of this stage written by commits
  uint64_t im_load_address_ok = 0;   // ... whose address is the one computed from the object's own fields
  uint64_t ucode_checked = 0;        // IM_LOADs compared with the container's microcode
  uint64_t ucode_identical = 0;
  uint64_t ucode_layout_ok = 0;      // microcode location and size computed from the object equal what the container said at creation
  std::map<int, uint64_t> diff_words;  // histogram of differing 32-bit words (post-patch microcode vs container), bucketed
  std::unordered_set<uint64_t> distinct_containers;
  std::unordered_set<uint64_t> distinct_containers_in_cache;
};

int Bucket(uint32_t diff) { return diff == 0 ? 0 : diff <= 2 ? 2 : diff <= 6 ? 6 : diff <= 18 ? 18 : 19; }

class ShaderObserverImpl final : public ShaderObserver {
 public:
  ShaderObserverImpl(std::unique_ptr<ShaderCache> cache, std::filesystem::path report)
      : cache_(std::move(cache)), report_(std::move(report)), gfx_(getenv("GH2_D3D_VERIFY") != nullptr && getenv("GH2_D3D_VERIFY")[0] == '1') {
    t0_ = std::chrono::steady_clock::now();
    reporter_ = std::thread([this] {
      int next = 10;
      while (!stop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
        if (t < next) continue;
        next += 5;
        Report(t, (next - 5) % 10 == 0);
      }
    });
    fprintf(stderr, "D3DOBS observing the game's D3D library; shader cache with %zu shaders\n", cache_->size());
  }
  ~ShaderObserverImpl() override {
    stop_ = true;
    if (reporter_.joinable()) reporter_.join();
    Report(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count(), false);  // the final report
  }

  void OnShaderCreated(const uint8_t* base, Stage stage, uint32_t container, uint32_t object) override {
    if (!object || !container) return;
    const uint8_t* c = base + container;
    const uint32_t flags = Be32(base, container + container::kFlags);
    const uint32_t virtual_size = Be32(base, container + container::kVirtualSize);
    const uint32_t physical_size = Be32(base, container + container::kPhysicalSize);
    const uint32_t shader_offset = Be32(base, container + container::kShaderOffset);
    std::lock_guard<std::mutex> lock(mu_);
    if ((flags & 0xFFFFFF00u) != 0x102A1100u || virtual_size > (16u << 20) || physical_size > (16u << 20) || shader_offset + 8 > virtual_size) {
      ++bad_containers_;
      return;
    }
    if (((flags & 1u) != 0) != (stage == Stage::kVertex)) ++stage_mismatch_;
    const size_t total = static_cast<size_t>(virtual_size) + physical_size;
    const uint64_t hash = ShaderCache::HashContainer(c, total);
    auto& slot = originals_[hash];
    if (!slot) {
      auto o = std::make_shared<Original>();
      o->hash = hash;
      o->stage = stage;
      o->physical_offset = Be32(base, container + shader_offset);
      const uint32_t bytes = Be32(base, container + shader_offset + 4);
      if (o->physical_offset + static_cast<uint64_t>(bytes) <= physical_size) {
        const uint8_t* u = c + virtual_size + o->physical_offset;
        o->ucode.resize(bytes / 4);
        for (size_t i = 0; i < o->ucode.size(); ++i) {
          uint32_t w;
          std::memcpy(&w, u + i * 4, 4);
          o->ucode[i] = __builtin_bswap32(w);
        }
      }
      o->in_cache = cache_->Find(hash).has_value();
      if (!o->in_cache) {
        char context[96];
        snprintf(context, sizeof(context), "d3d create, container at guest %08X, object %08X", container, object);
        cache_->ReportUnknown(hash, stage == Stage::kVertex ? ShaderStage::kVertex : ShaderStage::kPixel, c, total, context);
      }
      slot = std::move(o);
    }
    objects_[object] = Object{slot, container};
    StageStats& st = stats_[static_cast<int>(stage)];
    ++st.created;
    st.distinct_containers.insert(hash);
    if (slot->in_cache) st.distinct_containers_in_cache.insert(hash);
  }

  BoundShaders Resolve(const uint8_t* base, uint32_t device) override {
    BoundShaders out;
    BoundShader* slots[2] = {&out.vertex, &out.pixel};
    const uint32_t objs[2] = {Be32(base, device + layout::kDeviceVertexShader), Be32(base, device + layout::kDevicePixelShader)};
    std::lock_guard<std::mutex> lock(mu_);
    for (int s = 0; s < 2; ++s) {
      slots[s]->object = objs[s];
      auto it = objects_.find(objs[s]);
      if (!objs[s] || it == objects_.end()) continue;
      slots[s]->registered = true;
      slots[s]->container_hash = it->second.original->hash;
      slots[s]->spirv = cache_->Find(it->second.original->hash);
    }
    return out;
  }

  void OnCall(const uint8_t* base, const Call& call) override { gfx_.OnCall(base, call); }

  void OnDrawStateCommit(const uint8_t* base, uint32_t device, uint32_t cmd_before, uint32_t cmd_after) override {
    const uint32_t objs[2] = {Be32(base, device + layout::kDeviceVertexShader), Be32(base, device + layout::kDevicePixelShader)};
    std::lock_guard<std::mutex> lock(mu_);
    ++commits_;
    uint64_t pair[2] = {0, 0};
    for (int s = 0; s < 2; ++s) {
      if (!objs[s]) continue;
      StageStats& st = stats_[s];
      ++st.bound_draws;
      auto it = objects_.find(objs[s]);
      if (it == objects_.end()) {
        ++unregistered_draws_;
        if (unregistered_.insert(objs[s]).second && unregistered_.size() <= 20)
          fprintf(stderr, "D3DOBS a %s shader object %08X was bound that the hooked creation functions never returned\n", s ? "pixel" : "vertex", objs[s]);
        continue;
      }
      ++st.registered_draws;
      if (it->second.original->in_cache) ++st.cache_hit_draws;
      pair[s] = it->second.original->hash;
    }
    if (pair[0] || pair[1]) ++pairs_[{pair[0], pair[1]}];
    PublishPipeline(base, device, objs, pair);
    if (cmd_after > cmd_before && cmd_after - cmd_before <= 4096 && ((cmd_after | cmd_before) & 3) == 0) ScanPackets(base, objs, cmd_before, cmd_after);
  }

 private:
  // What the microcode now in memory reads (vertex: as patched to the current declaration), cached per (object, declaration).
  void PublishPipeline(const uint8_t* base, uint32_t device, const uint32_t (&objs)[2], const uint64_t (&hashes)[2]) {
    PipelineInfo p;
    const uint32_t decl = Be32(base, device + layout::kDeviceVertexDeclaration);
    for (int s = 0; s < 2; ++s) {
      auto it = objects_.find(objs[s]);
      if (it == objects_.end()) continue;
      const Original& orig = *it->second.original;
      const uint64_t key = (static_cast<uint64_t>(objs[s]) << 32) | (s == 0 ? decl : 0);
      auto& slot = usage_[s][key];
      if (!slot) {
        const uint32_t copy = s == 0 ? layout::kVertexContainerCopy : layout::kPixelContainerCopy;
        const uint32_t ptr_field = s == 0 ? layout::kVertexUcodePointer : layout::kPixelUcodePointer;
        const uint32_t shader_offset = Be32(base, objs[s] + copy + container::kShaderOffset);
        const uint32_t virt = Be32(base, objs[s] + ptr_field) + Be32(base, objs[s] + copy + shader_offset);
        std::vector<uint32_t> words(orig.ucode.size());
        for (size_t i = 0; i < words.size(); ++i) words[i] = Be32(base, virt + static_cast<uint32_t>(i * 4));
        slot = std::make_shared<const FetchUsage>(ScanFetches(words.data(), words.size()));
      }
      (s == 0 ? p.vertex : p.pixel) = slot;
      (s == 0 ? p.vertex_hash : p.pixel_hash) = hashes[s];
    }
    p.valid = p.vertex || p.pixel;
    gfx_.OnPipeline(device, p);
  }

  // The dwords written by one commit: PM4 packets. IM_LOAD (0x27): dword1 = physical address | shader type (0 vertex, 1 pixel), dword2 = start << 16 | size in dwords.
  void ScanPackets(const uint8_t* base, const uint32_t (&objs)[2], uint32_t before, uint32_t after) {
    for (uint32_t p = before + 4; p <= after;) {
      const uint32_t header = Be32(base, p);
      const uint32_t type = header >> 30;
      if (type == 2) {
        p += 4;
        continue;
      }
      if (type == 1) {
        p += 8;
        continue;
      }
      const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
      if (type == 3 && ((header >> 8) & 0xFF) == kPm4ImLoad && count >= 2 && p + 12 <= after + 4) {
        const uint32_t address_type = Be32(base, p + 4);
        const uint32_t start_size = Be32(base, p + 8);
        CheckImLoad(base, objs, address_type, start_size);
      }
      p += 4 * (1 + count);
    }
  }

  void CheckImLoad(const uint8_t* base, const uint32_t (&objs)[2], uint32_t address_type, uint32_t start_size) {
    const int s = (address_type & 3) == 0 ? 0 : (address_type & 3) == 1 ? 1 : -1;  // 0 vertex, 1 pixel
    if (s < 0) return;
    StageStats& st = stats_[s];
    ++st.im_loads;
    auto it = objects_.find(objs[s]);
    if (it == objects_.end()) return;
    const Original& orig = *it->second.original;
    const uint32_t copy = s == 0 ? layout::kVertexContainerCopy : layout::kPixelContainerCopy;
    const uint32_t ucode_ptr_field = s == 0 ? layout::kVertexUcodePointer : layout::kPixelUcodePointer;
    // the microcode's place as the object itself says it (the library's own computation), and what the container said at creation
    const uint32_t shader_offset = Be32(base, objs[s] + copy + container::kShaderOffset);
    const uint32_t physical_offset = Be32(base, objs[s] + copy + shader_offset);
    const uint32_t bytes = Be32(base, objs[s] + copy + shader_offset + 4);
    const bool layout_ok = physical_offset == orig.physical_offset && bytes == orig.ucode.size() * 4;
    if (layout_ok) ++st.ucode_layout_ok;
    const uint32_t virt = Be32(base, objs[s] + ucode_ptr_field) + physical_offset;
    if ((address_type & ~3u) == (PhysicalOf(virt) & ~3u) && (start_size & 0xFFFF) == bytes / 4) ++st.im_load_address_ok;
    if (!layout_ok) return;
    // the microcode as it sits in memory now
    ++st.ucode_checked;
    uint32_t diff = 0;
    for (size_t i = 0; i < orig.ucode.size(); ++i)
      if (Be32(base, virt + static_cast<uint32_t>(i * 4)) != orig.ucode[i]) ++diff;
    if (diff == 0) ++st.ucode_identical;
    ++st.diff_words[Bucket(diff)];
    if (diff != 0 && patch_samples_left_ > 0) {
      std::vector<uint32_t> now(orig.ucode.size());
      for (size_t i = 0; i < now.size(); ++i) now[i] = Be32(base, virt + static_cast<uint32_t>(i * 4));
      const uint64_t signature = ShaderCache::HashContainer(now.data(), now.size() * 4) ^ orig.hash;
      if (patch_seen_.insert(signature).second) {
        --patch_samples_left_;
        std::string line;
        char word[80];
        for (size_t i = 0; i < now.size(); ++i)
          if (now[i] != orig.ucode[i]) {
            snprintf(word, sizeof(word), " [%zu: %08X -> %08X, xor %08X]", i, orig.ucode[i], now[i], orig.ucode[i] ^ now[i]);
            line += word;
          }
        fprintf(stderr, "D3DOBS patch of %s shader %016llX (%zu words):%s\n", s == 0 ? "vertex" : "pixel", static_cast<unsigned long long>(orig.hash), now.size(), line.c_str());
      }
    }
  }

  void Report(double t, bool print) {
    std::lock_guard<std::mutex> lock(mu_);
    const char* names[2] = {"vertex", "pixel"};
    char line[400];
    const auto& v = stats_[0];
    const auto& p = stats_[1];
    snprintf(line, sizeof(line),
             "D3DOBS T+%.0fs commits %llu | created: vs %llu (%zu containers, %zu in cache), ps %llu (%zu, %zu) | draws with a registered shader: vs %llu/%llu ps %llu/%llu, in cache: vs %llu ps %llu | "
             "unregistered objects bound: %zu (%llu draws)",
             t, static_cast<unsigned long long>(commits_), static_cast<unsigned long long>(v.created), v.distinct_containers.size(), v.distinct_containers_in_cache.size(),
             static_cast<unsigned long long>(p.created), p.distinct_containers.size(), p.distinct_containers_in_cache.size(), static_cast<unsigned long long>(v.registered_draws),
             static_cast<unsigned long long>(v.bound_draws), static_cast<unsigned long long>(p.registered_draws), static_cast<unsigned long long>(p.bound_draws),
             static_cast<unsigned long long>(v.cache_hit_draws), static_cast<unsigned long long>(p.cache_hit_draws), unregistered_.size(),
             static_cast<unsigned long long>(unregistered_draws_));
    if (print) {
      fprintf(stderr, "%s\n", line);
      fprintf(stderr, "%s\n", gfx_.SummaryLine(t).c_str());
    }
    if (report_.empty()) return;
    if (FILE* f = fopen(report_.c_str(), "w")) {
      fprintf(f, "{\n \"seconds\": %.1f,\n \"commits\": %llu,\n \"bad_containers\": %llu,\n \"stage_mismatch\": %llu,\n \"unregistered_objects\": %zu,\n \"unregistered_draws\": %llu,\n",
              t, static_cast<unsigned long long>(commits_), static_cast<unsigned long long>(bad_containers_), static_cast<unsigned long long>(stage_mismatch_),
              unregistered_.size(), static_cast<unsigned long long>(unregistered_draws_));
      fprintf(f, " \"distinct_vs_ps_pairs\": %zu,\n", pairs_.size());
      for (int s = 0; s < 2; ++s) {
        const StageStats& st = stats_[s];
        fprintf(f,
                " \"%s\": {\"created\": %llu, \"distinct_containers\": %zu, \"distinct_containers_in_cache\": %zu, \"bound_draws\": %llu, \"registered_draws\": %llu, "
                "\"cache_hit_draws\": %llu, \"im_loads\": %llu, \"im_load_address_ok\": %llu, \"ucode_layout_ok\": %llu, \"ucode_checked\": %llu, "
                "\"ucode_identical\": %llu, \"diff_words_buckets\": {",
                names[s], static_cast<unsigned long long>(st.created), st.distinct_containers.size(), st.distinct_containers_in_cache.size(),
                static_cast<unsigned long long>(st.bound_draws), static_cast<unsigned long long>(st.registered_draws), static_cast<unsigned long long>(st.cache_hit_draws),
                static_cast<unsigned long long>(st.im_loads), static_cast<unsigned long long>(st.im_load_address_ok), static_cast<unsigned long long>(st.ucode_layout_ok),
                static_cast<unsigned long long>(st.ucode_checked), static_cast<unsigned long long>(st.ucode_identical));
        bool first = true;
        for (const auto& [bucket, n] : st.diff_words) {
          fprintf(f, "%s\"%s\": %llu", first ? "" : ", ", bucket == 0 ? "0" : bucket == 2 ? "1-2" : bucket == 6 ? "3-6" : bucket == 18 ? "7-18" : "19+", static_cast<unsigned long long>(n));
          first = false;
        }
        fprintf(f, "}}%s\n", s == 0 ? "," : "");
      }
      fprintf(f, " ,\"graphics\": ");
      gfx_.WriteJson(f, t);
      fprintf(f, "}\n");
      fclose(f);
    }
  }

  std::unique_ptr<ShaderCache> cache_;
  std::filesystem::path report_;
  GraphicsState gfx_;
  std::unordered_map<uint64_t, std::shared_ptr<const FetchUsage>> usage_[2];
  int patch_samples_left_ = [] {
    const char* e = getenv("GH2_D3D_DUMP_PATCHES");  // log the words the library rewrites in this many distinct vertex shaders
    return e ? atoi(e) : 0;
  }();
  std::unordered_set<uint64_t> patch_seen_;
  std::mutex mu_;
  std::unordered_map<uint32_t, Object> objects_;
  std::unordered_map<uint64_t, std::shared_ptr<Original>> originals_;
  std::unordered_set<uint32_t> unregistered_;
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> pairs_;
  StageStats stats_[2];
  uint64_t commits_ = 0, unregistered_draws_ = 0, bad_containers_ = 0, stage_mismatch_ = 0;
  std::chrono::steady_clock::time_point t0_;
  std::thread reporter_;
  std::atomic<bool> stop_{false};
};

}  // namespace

std::unique_ptr<ShaderObserver> MakeShaderObserver(const std::filesystem::path& cache_dir, const std::filesystem::path& report_json) {
  std::string error;
  auto cache = ShaderCache::Open(cache_dir, &error);
  if (!cache) {
    fprintf(stderr, "D3DOBS cannot open the shader cache %s: %s\n", cache_dir.c_str(), error.c_str());
    return nullptr;
  }
  return std::make_unique<ShaderObserverImpl>(std::move(cache), report_json);
}

}  // namespace gh2gfx::d3d
