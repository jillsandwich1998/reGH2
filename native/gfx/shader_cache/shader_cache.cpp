#include "shader_cache.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>

namespace gh2gfx {
namespace {

constexpr char kMagic[8] = {'G', 'H', '2', 'S', 'H', 'D', 'R', '1'};
constexpr uint32_t kVersion = 2;   // 2: vertex shaders carry dependent_mask (patch 0004). A version-1 cache draws the notes as shards without any error, so it is refused

#pragma pack(push, 1)
struct PakHeader {
  char magic[8];
  uint32_t version, count, entries_offset, blob_offset, blob_size, blob_crc32;
};
struct PakEntry {
  uint64_t hash;
  uint32_t offset, size, spec_mask;
  uint8_t stage, flags;
  uint16_t dependent_mask;   // vertex shaders: bit i = element i of the container's table is read through a dependent vertex fetch (patch 0004)
};
#pragma pack(pop)
static_assert(sizeof(PakHeader) == 32, "shaders.pak header layout");
static_assert(sizeof(PakEntry) == 24, "shaders.pak entry layout");

uint32_t Crc32(const uint8_t* p, size_t n) {
  static const auto table = [] {
    std::vector<uint32_t> t(256);
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

PakEntry EntryAt(const uint8_t* base, size_t i) {
  PakEntry e;
  std::memcpy(&e, base + i * sizeof(PakEntry), sizeof(e));
  return e;
}

}  // namespace

std::unique_ptr<ShaderCache> ShaderCache::Open(const std::filesystem::path& dir, std::string* error) {
  auto fail = [&](const std::string& why) -> std::unique_ptr<ShaderCache> {
    if (error) *error = why;
    return nullptr;
  };
  std::ifstream f(dir / "shaders.pak", std::ios::binary);
  if (!f) return fail("cannot open " + (dir / "shaders.pak").string());
  std::unique_ptr<ShaderCache> cache(new ShaderCache());
  cache->dir_ = dir;
  cache->file_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  const std::vector<uint8_t>& d = cache->file_;
  if (d.size() < sizeof(PakHeader)) return fail("shaders.pak is too short");
  PakHeader h;
  std::memcpy(&h, d.data(), sizeof(h));
  if (std::memcmp(h.magic, kMagic, sizeof(kMagic)) != 0) return fail("shaders.pak: bad magic");
  if (h.version == 1) return fail("shaders.pak is version 1, made before the dependent vertex fetch patch (0004): the notes would be drawn as shards. Rebuild it with native/tools/shader_pipeline.py");
  if (h.version != kVersion) return fail("shaders.pak: unsupported version " + std::to_string(h.version));
  const uint64_t entries_end = uint64_t(h.entries_offset) + uint64_t(h.count) * sizeof(PakEntry);
  if (h.entries_offset < sizeof(PakHeader) || entries_end > d.size()) return fail("shaders.pak: entry table out of range");
  if (uint64_t(h.blob_offset) + h.blob_size > d.size() || h.blob_offset < entries_end) return fail("shaders.pak: blob out of range");
  if (Crc32(d.data() + h.blob_offset, h.blob_size) != h.blob_crc32) return fail("shaders.pak: blob checksum mismatch");
  cache->entries_ = d.data() + h.entries_offset;
  cache->blob_ = d.data() + h.blob_offset;
  cache->count_ = h.count;
  cache->blob_size_ = h.blob_size;
  uint64_t previous = 0;
  for (size_t i = 0; i < cache->count_; ++i) {
    const PakEntry e = EntryAt(cache->entries_, i);
    if (i > 0 && e.hash <= previous) return fail("shaders.pak: entries are not sorted by hash");
    if (uint64_t(e.offset) + e.size > cache->blob_size_ || (e.offset & 3) != 0 || (e.size & 3) != 0 || e.size < 20)
      return fail("shaders.pak: entry " + std::to_string(i) + " is out of range or misaligned");
    previous = e.hash;
  }
  return cache;
}

std::optional<ShaderBlob> ShaderCache::Find(uint64_t hash) const {
  lookups_.fetch_add(1, std::memory_order_relaxed);
  size_t lo = 0, hi = count_;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const PakEntry e = EntryAt(entries_, mid);
    if (e.hash == hash) {
      hits_.fetch_add(1, std::memory_order_relaxed);
      ShaderBlob b;
      b.spirv = reinterpret_cast<const uint32_t*>(blob_ + e.offset);
      b.size_bytes = e.size;
      b.spec_mask = e.spec_mask;
      b.dependent_mask = e.dependent_mask;
      b.stage = e.stage == 0 ? ShaderStage::kVertex : ShaderStage::kPixel;
      return b;
    }
    (e.hash < hash ? lo = mid + 1 : hi = mid);
  }
  misses_.fetch_add(1, std::memory_order_relaxed);
  return std::nullopt;
}

ShaderCacheStats ShaderCache::stats() const {
  ShaderCacheStats s;
  s.lookups = lookups_.load();
  s.hits = hits_.load();
  s.misses = misses_.load();
  {
    auto* self = const_cast<ShaderCache*>(this);
    std::lock_guard<std::mutex> lock(self->unknown_mutex_);
    s.unique_unknown = unknown_.size();
  }
  return s;
}

bool ShaderCache::ParseContainerHeader(const void* data, size_t available, size_t* container_size, ShaderStage* stage) {
  constexpr size_t kHeaderBytes = 36;  // flags, virtualSize, physicalSize, fieldC, constantTable, definitionTable, shader, field1C, field20
  if (available < kHeaderBytes) return false;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  const uint32_t flags = Be32(p), virtual_size = Be32(p + 4), physical_size = Be32(p + 8);
  const uint32_t field1c = Be32(p + 28), field20 = Be32(p + 32);
  const uint64_t total = uint64_t(virtual_size) + physical_size;
  if ((flags & 0xFFFFFF00u) != 0x102A1100u || total > available || total < kHeaderBytes || field1c != 0 || field20 != 0) return false;
  if (container_size) *container_size = static_cast<size_t>(total);
  if (stage) *stage = (flags & 1) == 0 ? ShaderStage::kPixel : ShaderStage::kVertex;
  return true;
}

uint64_t ShaderCache::HashContainer(const void* data, size_t container_size) { return XXH3_64bits(data, container_size); }

std::optional<ShaderBlob> ShaderCache::FindContainer(const void* data, size_t available, const char* context) {
  size_t size = 0;
  ShaderStage stage = ShaderStage::kVertex;
  if (!ParseContainerHeader(data, available, &size, &stage)) {
    ReportUnknown(0, stage, data, 0, "not a shader container");
    return std::nullopt;
  }
  const uint64_t hash = HashContainer(data, size);
  if (auto b = Find(hash)) return b;
  ReportUnknown(hash, stage, data, size, context);
  return std::nullopt;
}

void ShaderCache::ReportUnknown(uint64_t hash, ShaderStage stage, const void* container, size_t container_size, const char* context) {
  {
    std::lock_guard<std::mutex> lock(unknown_mutex_);
    if (!unknown_.insert(hash).second) return;  // once per process per hash
  }
  char when[32];
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  gmtime_r(&t, &tm);
  std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
  char line[512];
  std::snprintf(line, sizeof(line), "%s %016llX %s %zu %s\n", when, static_cast<unsigned long long>(hash),
                stage == ShaderStage::kPixel ? "ps" : "vs", container_size, context ? context : "");
  {
    std::lock_guard<std::mutex> lock(unknown_mutex_);  // serializes the file appends too
    if (FILE* f = std::fopen((dir_ / "unknown_hashes.log").string().c_str(), "a")) {
      std::fputs(line, f);
      std::fclose(f);
    }
  }
  const char* dump = std::getenv("GH2_SHADER_DUMP_UNKNOWN");
  if (dump && dump[0] != '0' && container && container_size) {
    std::error_code ec;
    std::filesystem::create_directories(dir_ / "unknown", ec);
    char name[32];
    std::snprintf(name, sizeof(name), "%016llX.fxc", static_cast<unsigned long long>(hash));
    if (FILE* f = std::fopen((dir_ / "unknown" / name).string().c_str(), "wb")) {
      std::fwrite(container, 1, container_size, f);
      std::fclose(f);
    }
  }
}

}  // namespace gh2gfx
