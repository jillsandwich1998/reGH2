// Positional, read-only file access for the VFS and the ARK reader: open, size, read at an offset (no shared file position, safe from several threads), close.
// POSIX: open/fstat/pread. Windows: CreateFileW/GetFileInformationByHandle/ReadFile with an OVERLAPPED offset on a synchronous handle. Header-only, private to gh2k_vfs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gh2k::fio {

#ifdef _WIN32
using Handle = HANDLE;
inline const Handle kInvalid = INVALID_HANDLE_VALUE;
inline bool Valid(Handle h) { return h != INVALID_HANDLE_VALUE; }

// Opens a file or a directory for reading. On failure *access_denied tells "denied" from "not found / other".
inline Handle OpenRead(const std::filesystem::path& path, bool* access_denied = nullptr) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE && access_denied) *access_denied = GetLastError() == ERROR_ACCESS_DENIED;
  return h;
}

inline bool Stat(Handle h, uint64_t* size, bool* directory) {
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(h, &info)) return false;
  *size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
  *directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  return true;
}

// Reads up to `length` bytes at `offset`; returns the count (0 at the end of the file), or -1 on an error. Large reads are made in pieces below 2 GB.
inline int64_t ReadAt(Handle h, void* buffer, size_t length, uint64_t offset) {
  const DWORD chunk = static_cast<DWORD>(length > 0x40000000u ? 0x40000000u : length);
  OVERLAPPED ov = {};
  ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
  ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
  DWORD got = 0;
  if (!ReadFile(h, buffer, chunk, &got, &ov)) return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
  return got;
}

inline void Close(Handle h) {
  if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}
#else
using Handle = int;
constexpr Handle kInvalid = -1;
inline bool Valid(Handle h) { return h >= 0; }

inline Handle OpenRead(const std::filesystem::path& path, bool* access_denied = nullptr) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0 && access_denied) *access_denied = errno == EACCES;
  return fd;
}

inline bool Stat(Handle h, uint64_t* size, bool* directory) {
  struct stat st;
  if (::fstat(h, &st) != 0) return false;
  *size = static_cast<uint64_t>(st.st_size);
  *directory = S_ISDIR(st.st_mode);
  return true;
}

inline int64_t ReadAt(Handle h, void* buffer, size_t length, uint64_t offset) {
  for (;;) {
    const ssize_t n = ::pread(h, buffer, length, static_cast<off_t>(offset));
    if (n < 0 && errno == EINTR) continue;
    return n;
  }
}

inline void Close(Handle h) {
  if (h >= 0) ::close(h);
}
#endif

}  // namespace gh2k::fio
