// gh2gfx::ShaderCache - the on-disk cache of translated shaders (SPIR-V), keyed by the hash of the guest's shader container.
//
// Produced at install time on the player's machine by native/tools/shader_pipeline.py (the shaders are extracted from the player's own
// ARK, translated by XenosRecomp, compiled by DXC and validated by spirv-val; nothing here is committed to the repository). The cache
// directory holds shaders.pak (this file's format, below), manifest.json (provenance, counts) and index.tsv; at run time
// unknown_hashes.log collects the containers the game asked for that the cache did not have.
//
// Hash = XXH3_64bits over the whole container (virtualSize + physicalSize bytes, big-endian header fields): the same definition
// XenosRecomp uses, so the renderer hashes the container the game hands to CreateVertexShader/CreatePixelShader the same way.
//
// shaders.pak, little-endian:
//   header (32 bytes): "GH2SHDR1", u32 version = 2, u32 count, u32 entries_offset, u32 blob_offset, u32 blob_size, u32 blob_crc32
//   count entries (24 bytes) sorted by hash: u64 hash, u32 offset (in the blob), u32 size, u32 spec_mask, u8 stage (0 vs, 1 ps), u8 flags,
//   u16 dependent_mask (vertex shaders: bit i = element i of the container's vertex element table is fetched with a shader-computed index, see patch 0004)
//   blob: the SPIR-V modules, each 4-byte aligned
//
// No dependency on the SDK, the shim or the renderer.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace gh2gfx {

enum class ShaderStage : uint8_t { kVertex = 0, kPixel = 1 };

struct ShaderBlob {
  const uint32_t* spirv = nullptr;  // valid while the cache lives
  size_t size_bytes = 0;
  uint32_t spec_mask = 0;           // which specialization constants the shader uses (bit 0: R11G11B10 normals, bit 1: alpha test)
  uint32_t dependent_mask = 0;      // vertex shaders: elements read by dependent vertex fetches (the renderer describes their streams in the shared constants instead of binding attributes)
  ShaderStage stage = ShaderStage::kVertex;
};

struct ShaderCacheStats {
  uint64_t lookups = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t unique_unknown = 0;
};

class ShaderCache {
 public:
  // Opens <dir>/shaders.pak. On failure returns null and fills `error`.
  static std::unique_ptr<ShaderCache> Open(const std::filesystem::path& dir, std::string* error = nullptr);

  // Thread-safe and lock-free (the cache is immutable once opened).
  std::optional<ShaderBlob> Find(uint64_t hash) const;
  size_t size() const { return count_; }
  ShaderCacheStats stats() const;

  // Guest shader containers (what XenosRecomp scans for): header magic 0x102A11xx, sizes inside the available bytes.
  // bit 0 of the flags selects the stage (0 = pixel, 1 = vertex), as in XenosRecomp.
  static bool ParseContainerHeader(const void* data, size_t available, size_t* container_size, ShaderStage* stage);
  static uint64_t HashContainer(const void* data, size_t container_size);

  // Looks a container up; on a miss records the hash (see ReportUnknown) and returns an empty optional.
  std::optional<ShaderBlob> FindContainer(const void* data, size_t available, const char* context = "");

  // Records a hash the cache does not have: once per process per hash, appended to <dir>/unknown_hashes.log as
  //   <UTC time> <hash> <vs|ps> <container bytes> <context>
  // With GH2_SHADER_DUMP_UNKNOWN=1 the container bytes are also saved to <dir>/unknown/<hash>.fxc so the next install can add it.
  void ReportUnknown(uint64_t hash, ShaderStage stage, const void* container, size_t container_size, const char* context);

 private:
  ShaderCache() = default;

  std::filesystem::path dir_;
  std::vector<uint8_t> file_;
  const uint8_t* entries_ = nullptr;
  const uint8_t* blob_ = nullptr;
  size_t count_ = 0;
  size_t blob_size_ = 0;

  mutable std::atomic<uint64_t> lookups_{0}, hits_{0}, misses_{0};
  std::mutex unknown_mutex_;
  std::unordered_set<uint64_t> unknown_;
};

}  // namespace gh2gfx
