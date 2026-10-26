// gh2_vfs_selftest - the native VFS and ARK reader.
//
//   gh2_vfs_selftest                       synthetic checks in a temp directory (paths, case, positions, end of file)
//   gh2_vfs_selftest GAME_DIR [MANIFEST]   also the player's real game folder: d:\gen\main.hdr through the VFS, the ARK header decrypted and parsed natively;
//                                          MANIFEST (a path) gets one line per ARK entry: path, offset, size, crc32 of its bytes (compare with ark_manifest.py)
// Exit status 0 = everything passed.
#include <unistd.h>
#include <zlib.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "gh2k/ark.h"
#include "gh2k/vfs.h"

namespace fs = std::filesystem;
using namespace gh2k;

namespace {
int g_failures = 0;
void Check(bool ok, const std::string& what, const std::string& detail = "") {
  std::printf("[%s] %s%s%s\n", ok ? "ok" : "!!", what.c_str(), detail.empty() ? "" : " - ", detail.c_str());
  if (!ok) ++g_failures;
}

void Synthetic() {
  const fs::path tmp = fs::temp_directory_path() / ("gh2_vfs_selftest_" + std::to_string(::getpid()));
  fs::create_directories(tmp / "Gen" / "Sub");
  { std::ofstream(tmp / "Gen" / "Main.HDR", std::ios::binary) << "0123456789ABCDEF"; }
  { std::ofstream(tmp / "readme.txt", std::ios::binary) << "x"; }
  Vfs vfs;
  vfs.Mount("d:", tmp);
  vfs.Mount("game:", tmp);
  vfs.Mount("\\Device\\Cdrom0", tmp);
  Check(vfs.Claims("d:\\gen\\main.hdr") && vfs.Claims("GAME:\\x") && vfs.Claims("\\??\\D:\\gen") && vfs.Claims("\\Device\\Cdrom0\\gen\\main.hdr"), "paths on a mounted device are claimed (d:, game:, \\??\\D:, \\Device\\Cdrom0)");
  Check(!vfs.Claims("save:\\x") && !vfs.Claims("e:\\x") && !vfs.Claims("d:gen"), "other devices and \"d:gen\" are not");
  NtStatus st;
  auto p = vfs.Resolve("d:\\GEN\\main.hdr", &st);
  Check(p && p->filename() == "Main.HDR", "case-insensitive: d:\\GEN\\main.hdr -> Gen/Main.HDR");
  p = vfs.Resolve("D:\\gen\\.\\sub\\..\\main.hdr", &st);
  Check(p && p->filename() == "Main.HDR", "\".\" and \"..\" inside the root are resolved");
  p = vfs.Resolve("d:\\..\\etc\\passwd", &st);
  Check(!p, "\"..\" cannot leave the root");
  p = vfs.Resolve("d:\\gen\\nope.bin", &st);
  Check(!p && st == kStatusObjectNameNotFound, "a missing file is OBJECT_NAME_NOT_FOUND");
  p = vfs.Resolve("d:\\nodir\\nope.bin", &st);
  Check(!p && st == kStatusObjectPathNotFound, "a missing directory is OBJECT_PATH_NOT_FOUND");

  uint32_t h = 0, n = 0;
  Check(vfs.Open("d:\\gen\\main.hdr", &h) == kStatusSuccess && vfs.Owns(h) && h != 0 && h != 0xFFFFFFFFu, "open returns an owned handle");
  char buf[32] = {};
  Check(vfs.Read(h, 4, buf, 4, &n) == kStatusSuccess && n == 4 && std::memcmp(buf, "4567", 4) == 0, "positional read");
  Check(vfs.Read(h, kCurrentPosition, buf, 3, &n) == kStatusSuccess && std::memcmp(buf, "012", 3) == 0, "read at the handle's position (starts at 0)");
  Check(vfs.Read(h, kCurrentPosition, buf, 3, &n) == kStatusSuccess && std::memcmp(buf, "345", 3) == 0, "... and advances");
  Check(vfs.Read(h, 12, buf, 16, &n) == kStatusSuccess && n == 4, "a read that crosses the end returns what is there");
  Check(vfs.Read(h, 16, buf, 4, &n) == kStatusEndOfFile && n == 0, "a read at the end is END_OF_FILE");
  FileInfo info;
  Check(vfs.Info(h, &info) == kStatusSuccess && info.size == 16 && !info.directory, "size");
  Check(vfs.Close(h) == kStatusSuccess && vfs.Read(h, 0, buf, 1, &n) == kStatusInvalidHandle && vfs.Close(h) == kStatusInvalidHandle, "closed handles are invalid");
  Check(vfs.Open("d:\\gen\\zzz", &h) == kStatusObjectNameNotFound, "open of a missing file fails with the NT status");
  Check(vfs.Stats().opens == 1 && vfs.Stats().open_failures == 1 && vfs.Files().size() == 1 && vfs.Files()[0].bytes == 4 + 3 + 3 + 4, "counters and the per-file record");
  fs::remove_all(tmp);
}

uint32_t Crc(const std::vector<uint8_t>& d) { return static_cast<uint32_t>(crc32(0, d.data(), static_cast<uInt>(d.size()))); }

void Real(const fs::path& game, const char* manifest) {
  Vfs vfs;
  vfs.Mount("d:", game);
  uint32_t h = 0, n = 0;
  Check(vfs.Open("d:\\GEN\\MAIN.HDR", &h) == kStatusSuccess, "the VFS opens d:\\GEN\\MAIN.HDR (any case)");
  FileInfo info;
  vfs.Info(h, &info);
  std::vector<uint8_t> viavfs(info.size);
  Check(vfs.Read(h, 0, viavfs.data(), static_cast<uint32_t>(info.size), &n) == kStatusSuccess && n == info.size, "... and reads the whole header", std::to_string(info.size) + " bytes");
  std::ifstream f(game / "gen" / "main.hdr", std::ios::binary);
  std::vector<uint8_t> direct((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  Check(direct == viavfs, "bytes through the VFS equal the file's");
  vfs.Close(h);

  std::string error;
  const auto t0 = std::chrono::steady_clock::now();
  auto ark = ArkArchive::Open(game / "gen" / "main.hdr", &error);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  Check(ark != nullptr, "the ARK header decrypts and parses natively", error);
  if (!ark) return;
  std::map<std::string, size_t> ext;
  uint64_t stored = 0, compressed = 0;
  for (const ArkEntry& e : ark->entries()) {
    const size_t dot = e.path.rfind('.');
    ++ext[dot == std::string::npos ? "(none)" : e.path.substr(dot)];
    stored += e.size;
    compressed += e.inflated && e.inflated != e.size;
  }
  std::printf("    version %u, encrypted %d, %zu part(s), %zu entries, %.1f ms to open; stored %.2f GB, %llu compressed\n", ark->version(), ark->encrypted(), ark->part_sizes().size(),
              ark->entries().size(), ms, stored / 1e9, static_cast<unsigned long long>(compressed));
  std::printf("    ");
  for (const auto& [k, v] : ext)
    if (v >= 20) std::printf("%s=%zu ", k.c_str(), v);
  std::printf("\n");
  Check(ark->entries().size() == 7912, "7912 entries (what ark_extract.py reports)");
  const ArkEntry* any = ark->entries().empty() ? nullptr : &ark->entries().front();
  Check(any && ark->Find(any->path) == any, "Find by path");
  if (manifest) {
    std::FILE* out = std::fopen(manifest, "w");
    uint64_t bytes = 0;
    const auto t1 = std::chrono::steady_clock::now();
    for (const ArkEntry& e : ark->entries()) {
      std::vector<uint8_t> data;
      if (!ark->Read(e, &data, &error)) {
        Check(false, "read " + e.path, error);
        continue;
      }
      bytes += data.size();
      std::fprintf(out, "%s\t%llu\t%zu\t%08x\n", e.path.c_str(), static_cast<unsigned long long>(e.offset), data.size(), Crc(data));
    }
    std::fclose(out);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    std::printf("    read and checksummed every entry: %.2f GB in %.1f s\n", bytes / 1e9, s);
  }
}
}  // namespace

int main(int argc, char** argv) {
  Synthetic();
  if (argc > 1) Real(argv[1], argc > 2 ? argv[2] : nullptr);
  std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
