// gh2k/ark.h - Harmonix ARK archives (gen/main.hdr + main_N.ark), read natively (Phase 2.7, K.3).
//
// The same format ark_extract.py reads (written from the public description of the format, e.g. the MIT-licensed Mackiloha / ArkHelper project, then
// reimplemented): main.hdr is a version, or a 32-bit key followed by a stream encrypted with a Park-Miller generator; versions 3-7 hold the part sizes, a
// string blob + string index table and one entry per file (offset, name, directory, size, inflated size). Offsets are global across the parts.
// Nothing from the game is stored in the repository: this reads the player's own files.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gh2k {

struct ArkEntry {
  std::string path;            // "../../ark/..." style, as stored; directory + "/" + name
  uint64_t offset = 0;         // global
  uint32_t size = 0;           // bytes stored
  uint32_t inflated = 0;       // bytes after inflating (0 or == size: stored)
  uint32_t part = 0;
  uint64_t part_offset = 0;
};

class ArkArchive {
 public:
  // `header` is the path of main.hdr; the parts are main_0.ark... next to it. Returns null and fills `error` when the header is unsupported or corrupt.
  static std::unique_ptr<ArkArchive> Open(const std::filesystem::path& header, std::string* error);

  uint32_t version() const { return version_; }
  bool encrypted() const { return encrypted_; }
  const std::vector<uint64_t>& part_sizes() const { return part_sizes_; }
  const std::vector<ArkEntry>& entries() const { return entries_; }
  // Case-insensitive; "/" and "\" are the same.
  const ArkEntry* Find(std::string_view path) const;
  // The entry's bytes, inflated when stored deflated. false + `error` on a short read or a corrupt stream.
  bool Read(const ArkEntry& entry, std::vector<uint8_t>* out, std::string* error) const;
  // Reads a range of the entry's stored bytes into `buffer` (what the engine itself does: positional reads inside a part).
  bool ReadStored(const ArkEntry& entry, uint64_t offset, void* buffer, uint32_t length, std::string* error) const;
  std::filesystem::path PartPath(uint32_t part) const;

 private:
  ArkArchive() = default;
  std::filesystem::path header_;
  uint32_t version_ = 0;
  bool encrypted_ = false;
  std::vector<uint64_t> part_sizes_;
  std::vector<ArkEntry> entries_;
  std::unordered_map<std::string, size_t> by_path_;   // lower case, '/' separators
};

}  // namespace gh2k
