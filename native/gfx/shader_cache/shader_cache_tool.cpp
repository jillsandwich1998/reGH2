// gh2_shader_cache_tool - checks a shader cache directory (shaders.pak) and exercises the lookup / unknown-hash logging paths.
//
//   gh2_shader_cache_tool verify   CACHE_DIR
//   gh2_shader_cache_tool lookup   CACHE_DIR HASH_HEX
//   gh2_shader_cache_tool hash     FILE...                     containers found in each file, with their hashes (as XenosRecomp scans)
//   gh2_shader_cache_tool selftest CACHE_DIR INDEX_TSV [SHADER_DIR]
//       - every ok hash of INDEX_TSV must be found, with the right stage and a SPIR-V magic;
//       - with SHADER_DIR: every .fx_xbox file there is read from disk and looked up through FindContainer (the run-time path:
//         header check + XXH3 of the real container bytes) and must hit;
//       - synthetic unknown containers (a real one with a byte changed) must miss, be logged once, and leave the log with exactly
//         one line per distinct unknown hash. Works on a temporary copy of the cache, the real unknown_hashes.log is not touched.
// Exit status 0 = everything passed.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "shader_cache.h"

namespace fs = std::filesystem;
using gh2gfx::ShaderCache;
using gh2gfx::ShaderStage;

namespace {

std::vector<uint8_t> ReadFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

int Verify(const fs::path& dir) {
  std::string error;
  auto cache = ShaderCache::Open(dir, &error);
  if (!cache) {
    std::printf("verify: FAILED: %s\n", error.c_str());
    return 1;
  }
  size_t vs = 0, ps = 0, bad_magic = 0;
  uint64_t bytes = 0;
  // Walk the entries through Find() of every hash is not possible without the list: re-read the pak entries directly.
  const auto file = ReadFile(dir / "shaders.pak");
  uint32_t count, entries_offset, blob_offset;
  std::memcpy(&count, file.data() + 12, 4);
  std::memcpy(&entries_offset, file.data() + 16, 4);
  std::memcpy(&blob_offset, file.data() + 20, 4);
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t hash;
    std::memcpy(&hash, file.data() + entries_offset + i * 24, 8);
    auto b = cache->Find(hash);
    if (!b) {
      std::printf("verify: entry %u (hash %016llX) is not found by Find()\n", i, static_cast<unsigned long long>(hash));
      return 1;
    }
    (b->stage == ShaderStage::kVertex ? vs : ps)++;
    bytes += b->size_bytes;
    if (b->spirv[0] != 0x07230203u) ++bad_magic;
  }
  std::printf("verify: ok - %u shaders (%zu vertex, %zu pixel), %llu SPIR-V bytes, %zu with a bad SPIR-V magic\n", count, vs, ps,
              static_cast<unsigned long long>(bytes), bad_magic);
  return bad_magic ? 1 : 0;
}

int HashFiles(int argc, char** argv) {
  for (int i = 2; i < argc; ++i) {
    const auto d = ReadFile(argv[i]);
    size_t found = 0;
    for (size_t off = 0; off + 36 <= d.size();) {
      size_t size = 0;
      ShaderStage stage;
      if (ShaderCache::ParseContainerHeader(d.data() + off, d.size() - off, &size, &stage)) {
        std::printf("%s +%zu %016llX %s %zu\n", argv[i], off, static_cast<unsigned long long>(ShaderCache::HashContainer(d.data() + off, size)),
                    stage == ShaderStage::kPixel ? "ps" : "vs", size);
        off += size;
        ++found;
      } else {
        off += 4;
      }
    }
    if (!found) std::printf("%s: no shader container\n", argv[i]);
  }
  return 0;
}

int SelfTest(const fs::path& dir, const fs::path& index, const fs::path& shader_dir) {
  int failures = 0;
  auto check = [&](bool ok, const char* what, const std::string& detail = "") {
    std::printf("[%s] %s%s%s\n", ok ? "ok" : "!!", what, detail.empty() ? "" : " - ", detail.c_str());
    if (!ok) ++failures;
  };

  // work on a copy so that the real unknown_hashes.log stays untouched
  const fs::path tmp = fs::temp_directory_path() / ("gh2_shader_selftest_" + std::to_string(::getpid()));
  fs::create_directories(tmp);
  fs::copy_file(dir / "shaders.pak", tmp / "shaders.pak", fs::copy_options::overwrite_existing);
  std::string error;
  auto cache = ShaderCache::Open(tmp, &error);
  check(cache != nullptr, "open the cache", error);
  if (!cache) return 1;

  // 1) every ok hash of the index
  std::ifstream in(index);
  std::string line;
  std::getline(in, line);  // header
  size_t expected = 0, hit = 0, wrong_stage = 0, bad_magic = 0;
  while (std::getline(in, line)) {
    std::vector<std::string> cols;
    std::stringstream ss(line);
    for (std::string c; std::getline(ss, c, '\t');) cols.push_back(c);
    if (cols.size() < 3 || cols[2] != "ok") continue;
    ++expected;
    const uint64_t hash = std::strtoull(cols[0].c_str(), nullptr, 16);
    auto b = cache->Find(hash);
    if (!b) continue;
    ++hit;
    if ((b->stage == ShaderStage::kPixel) != (cols[1] == "ps")) ++wrong_stage;
    if (b->spirv[0] != 0x07230203u) ++bad_magic;
  }
  check(expected > 0 && hit == expected && wrong_stage == 0 && bad_magic == 0, "index hashes are all found, with the right stage",
        std::to_string(hit) + "/" + std::to_string(expected) + ", wrong stage " + std::to_string(wrong_stage) + ", bad magic " +
            std::to_string(bad_magic));

  // 2) the run-time path on the real container bytes
  std::vector<std::vector<uint8_t>> samples;
  if (!shader_dir.empty()) {
    size_t files = 0, found = 0;
    for (const auto& e : fs::directory_iterator(shader_dir)) {
      if (e.path().extension() != ".fx_xbox") continue;
      auto data = ReadFile(e.path());
      ++files;
      if (cache->FindContainer(data.data(), data.size(), e.path().filename().string().c_str())) ++found;
      if (samples.size() < 8) samples.push_back(std::move(data));
    }
    check(files > 0 && found == files, "every extracted .fx_xbox is found through FindContainer (header check + XXH3 of the file)",
          std::to_string(found) + "/" + std::to_string(files));
  }
  const auto before = cache->stats();
  check(before.unique_unknown == 0, "no unknown hash was logged for the real containers", std::to_string(before.unique_unknown));

  // 3) unknown containers: one byte changed in the last byte of a real container
  size_t distinct = 0;
  std::set<uint64_t> unknown_hashes;
  for (size_t round = 0; round < 2; ++round) {          // the second round must not add log lines
    for (size_t i = 0; i < samples.size(); ++i) {
      auto copy = samples[i];
      size_t size = 0;
      ShaderStage stage;
      if (!ShaderCache::ParseContainerHeader(copy.data(), copy.size(), &size, &stage)) continue;
      copy[size - 1] ^= 0x5A;
      const uint64_t h = ShaderCache::HashContainer(copy.data(), size);
      const bool miss = !cache->FindContainer(copy.data(), copy.size(), "selftest");
      if (!miss) ++failures;
      if (round == 0) {
        unknown_hashes.insert(h);
        ++distinct;
      }
    }
  }
  const auto after = cache->stats();
  std::ifstream log(tmp / "unknown_hashes.log");
  size_t lines = 0;
  std::string first;
  for (std::string l; std::getline(log, l);) {
    if (lines++ == 0) first = l;
  }
  check(!samples.empty() ? (after.unique_unknown == unknown_hashes.size() && lines == unknown_hashes.size()) : true,
        "unknown containers miss and are logged once each",
        std::to_string(unknown_hashes.size()) + " distinct unknown, " + std::to_string(lines) + " log lines, " +
            std::to_string(after.misses) + " misses counted");
  if (!first.empty()) std::printf("      log line example: %s\n", first.c_str());
  check(cache->Find(0x0123456789ABCDEFull) == std::nullopt, "an arbitrary hash is not found");
  std::printf("stats: lookups %llu, hits %llu, misses %llu, unique unknown %llu\n", static_cast<unsigned long long>(after.lookups),
              static_cast<unsigned long long>(after.hits), static_cast<unsigned long long>(after.misses),
              static_cast<unsigned long long>(after.unique_unknown));
  std::error_code ec;
  fs::remove_all(tmp, ec);
  std::printf("selftest: %s\n", failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && !std::strcmp(argv[1], "verify")) return Verify(argv[2]);
  if (argc >= 4 && !std::strcmp(argv[1], "lookup")) {
    std::string error;
    auto cache = ShaderCache::Open(argv[2], &error);
    if (!cache) return std::fprintf(stderr, "%s\n", error.c_str()), 1;
    auto b = cache->Find(std::strtoull(argv[3], nullptr, 16));
    if (!b) return std::puts("not found"), 1;
    std::printf("%s, %zu bytes, spec mask %u\n", b->stage == ShaderStage::kPixel ? "ps" : "vs", b->size_bytes, b->spec_mask);
    return 0;
  }
  if (argc >= 3 && !std::strcmp(argv[1], "hash")) return HashFiles(argc, argv);
  if (argc >= 4 && !std::strcmp(argv[1], "selftest")) return SelfTest(argv[2], argv[3], argc >= 5 ? argv[4] : "");
  std::fprintf(stderr,
               "usage: gh2_shader_cache_tool verify DIR | lookup DIR HASH | hash FILE... | selftest DIR INDEX_TSV [SHADER_DIR]\n");
  return 2;
}
