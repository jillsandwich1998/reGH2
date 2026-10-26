// gh2k/vfs.h - the game's file system, natively (Phase 2.7, K.3).
//
// The title opens its files through four kernel imports (NtCreateFile, NtReadFile, NtQueryInformationFile, NtClose; 46/869/44/67 calls in a song) and
// reads them by absolute offset: d:\gen\main.hdr, d:\gen\main_0.ark (2 GB, everything else lives inside it), d:\videos\*.wmv. This is the host side of
// those four calls: a mount table (guest device -> host directory), a case-insensitive path resolver (the Xbox file system ignores case, Linux does not),
// a handle table with read positions, and positional reads with pread(2). Read only: saves and content are not game files and stay with the SDK until
// their own step. Plain C++17, no SDK, no guest memory: the hooks (native/rt/hooks/hooks_vfs.cpp) translate between guest structures and this.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gh2k {

using NtStatus = uint32_t;
inline constexpr NtStatus kStatusSuccess = 0x00000000;
inline constexpr NtStatus kStatusInvalidHandle = 0xC0000008;
inline constexpr NtStatus kStatusEndOfFile = 0xC0000011;
inline constexpr NtStatus kStatusObjectNameNotFound = 0xC0000034;
inline constexpr NtStatus kStatusObjectPathNotFound = 0xC000003A;
inline constexpr NtStatus kStatusAccessDenied = 0xC0000022;
inline constexpr NtStatus kStatusNotADirectory = 0xC0000103;
inline constexpr NtStatus kStatusIoDeviceError = 0xC0000185;
inline constexpr uint64_t kCurrentPosition = ~0ull;   // "read at the handle's own position"

struct FileInfo {
  uint64_t size = 0;
  uint64_t position = 0;
  bool directory = false;
};

struct VfsStats {
  uint64_t opens = 0, open_failures = 0, reads = 0, bytes = 0, end_of_file_reads = 0, closes = 0;
};

// One file the game opened, kept after it is closed so a report can say what was read.
struct FileRecord {
  std::string guest_path;
  std::filesystem::path host_path;
  uint64_t size = 0;
  uint64_t opens = 0, reads = 0, bytes = 0;
};

class Vfs {
 public:
  Vfs();
  ~Vfs();
  Vfs(const Vfs&) = delete;
  Vfs& operator=(const Vfs&) = delete;

  // `device` is a guest prefix, case-insensitive, with or without the trailing separator: "d:", "game:", "\Device\Cdrom0".
  void Mount(std::string_view device, const std::filesystem::path& host_root);
  bool HasMounts() const { return !mounts_.empty(); }

  // Does a mount claim this guest path (d:\gen\main.hdr, \??\D:\gen\main.hdr, \Device\Cdrom0\gen\main.hdr, game:\...)? When it does, `host` is the real
  // file (nullopt when it does not exist, with `status` saying why).
  bool Claims(std::string_view guest_path) const;
  std::optional<std::filesystem::path> Resolve(std::string_view guest_path, NtStatus* status = nullptr);

  // Opens for reading. Handles are never 0 or 0xFFFFFFFF and never collide with the SDK's (they live in kHandleBase..).
  NtStatus Open(std::string_view guest_path, uint32_t* handle);
  NtStatus Read(uint32_t handle, uint64_t offset, void* buffer, uint32_t length, uint32_t* transferred);
  NtStatus Info(uint32_t handle, FileInfo* info);
  NtStatus SetPosition(uint32_t handle, uint64_t position);
  NtStatus Close(uint32_t handle);
  bool Owns(uint32_t handle) const { return handle >= kHandleBase && handle < kHandleBase + 0x01000000u; }

  VfsStats Stats() const;
  std::vector<FileRecord> Files() const;

  static constexpr uint32_t kHandleBase = 0x7E000000u;

 private:
  struct Mount_ {
    std::string device;                 // lower case, no trailing separator
    std::filesystem::path root;
  };
  struct Open_;
  std::shared_ptr<Open_> Find(uint32_t handle) const;
  // Splits a claimed guest path into components below the mount root.
  bool Split(std::string_view guest_path, const Mount_** mount, std::vector<std::string>* parts) const;
  std::optional<std::string> MatchName(const std::filesystem::path& dir, const std::string& name);

  std::vector<Mount_> mounts_;
  mutable std::mutex mu_;
  std::unordered_map<uint32_t, std::shared_ptr<Open_>> handles_;
  uint32_t next_ = 0;
  std::vector<std::shared_ptr<FileRecord>> records_;
  std::map<std::string, std::unordered_map<std::string, std::string>> dir_cache_;   // directory -> lower case name -> real name
  std::atomic<uint64_t> opens_{0}, open_failures_{0}, reads_{0}, bytes_{0}, eof_reads_{0}, closes_{0};
};

}  // namespace gh2k
