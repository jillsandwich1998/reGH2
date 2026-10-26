#include "gh2k/vfs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>

namespace fs = std::filesystem;

namespace gh2k {

namespace {

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string Slashes(std::string_view s) {
  std::string out(s);
  std::replace(out.begin(), out.end(), '\\', '/');
  return out;
}

}  // namespace

struct Vfs::Open_ {
  int fd = -1;
  bool directory = false;
  uint64_t size = 0;
  std::atomic<uint64_t> position{0};
  std::shared_ptr<FileRecord> record;
  ~Open_() {
    if (fd >= 0) ::close(fd);
  }
};

Vfs::Vfs() = default;
Vfs::~Vfs() = default;

void Vfs::Mount(std::string_view device, const fs::path& host_root) {
  std::string d = Lower(Slashes(device));
  while (!d.empty() && d.back() == '/') d.pop_back();
  mounts_.push_back({d, host_root});
}

bool Vfs::Split(std::string_view guest_path, const Mount_** mount, std::vector<std::string>* parts) const {
  const std::string p = Slashes(guest_path);
  const std::string lp = Lower(p);
  size_t skip = 0;
  if (lp.compare(0, 4, "/?\?/") == 0 || lp.compare(0, 4, "//?/") == 0) skip = 4;
  else if (lp.compare(0, 3, "?\?/") == 0) skip = 3;
  const std::string rest_l = lp.substr(skip);
  for (const Mount_& m : mounts_) {
    const std::string& d = m.device;
    if (rest_l.compare(0, d.size(), d) != 0) continue;
    if (rest_l.size() != d.size() && rest_l[d.size()] != '/') continue;
    const std::string rest = p.substr(skip + d.size());
    parts->clear();
    size_t i = 0;
    while (i <= rest.size()) {
      size_t j = rest.find('/', i);
      if (j == std::string::npos) j = rest.size();
      const std::string c = rest.substr(i, j - i);
      if (c == "..") {
        if (parts->empty()) return false;
        parts->pop_back();
      } else if (!c.empty() && c != ".") {
        parts->push_back(c);
      }
      i = j + 1;
    }
    *mount = &m;
    return true;
  }
  return false;
}

bool Vfs::Claims(std::string_view guest_path) const {
  const Mount_* m;
  std::vector<std::string> parts;
  return Split(guest_path, &m, &parts);
}

std::optional<std::string> Vfs::MatchName(const fs::path& dir, const std::string& name) {
  std::error_code ec;
  if (fs::exists(dir / name, ec)) return name;
  std::lock_guard<std::mutex> lock(mu_);
  auto& cache = dir_cache_[dir.string()];
  if (cache.empty()) {
    for (const auto& entry : fs::directory_iterator(dir, ec)) cache[Lower(entry.path().filename().string())] = entry.path().filename().string();
    if (cache.empty()) cache[""] = "";   // remember that the directory is empty / unreadable
  }
  auto it = cache.find(Lower(name));
  if (it == cache.end()) return std::nullopt;
  return it->second;
}

std::optional<fs::path> Vfs::Resolve(std::string_view guest_path, NtStatus* status) {
  const Mount_* m = nullptr;
  std::vector<std::string> parts;
  if (!Split(guest_path, &m, &parts)) {
    if (status) *status = kStatusObjectPathNotFound;
    return std::nullopt;
  }
  fs::path current = m->root;
  for (size_t i = 0; i < parts.size(); ++i) {
    std::error_code ec;
    if (!fs::is_directory(current, ec)) {
      if (status) *status = kStatusObjectPathNotFound;
      return std::nullopt;
    }
    auto name = MatchName(current, parts[i]);
    if (!name) {
      if (status) *status = (i + 1 == parts.size()) ? kStatusObjectNameNotFound : kStatusObjectPathNotFound;
      return std::nullopt;
    }
    current /= *name;
  }
  if (status) *status = kStatusSuccess;
  return current;
}

NtStatus Vfs::Open(std::string_view guest_path, uint32_t* handle) {
  NtStatus status = kStatusSuccess;
  auto host = Resolve(guest_path, &status);
  if (!host) {
    ++open_failures_;
    return status;
  }
  const int fd = ::open(host->c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    ++open_failures_;
    return errno == EACCES ? kStatusAccessDenied : kStatusObjectNameNotFound;
  }
  struct stat st;
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    ++open_failures_;
    return kStatusIoDeviceError;
  }
  auto file = std::make_shared<Open_>();
  file->fd = fd;
  file->directory = S_ISDIR(st.st_mode);
  file->size = static_cast<uint64_t>(st.st_size);
  std::lock_guard<std::mutex> lock(mu_);
  std::shared_ptr<FileRecord> rec;   // one record per distinct host file
  for (auto& r : records_)
    if (r->host_path == *host) rec = r;
  if (!rec) {
    rec = std::make_shared<FileRecord>();
    rec->guest_path = std::string(guest_path);
    rec->host_path = *host;
    rec->size = file->size;
    records_.push_back(rec);
  }
  ++rec->opens;
  file->record = rec;
  *handle = kHandleBase + 4 * (next_++);
  handles_[*handle] = file;
  ++opens_;
  return kStatusSuccess;
}

std::shared_ptr<Vfs::Open_> Vfs::Find(uint32_t handle) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = handles_.find(handle);
  return it == handles_.end() ? nullptr : it->second;
}

NtStatus Vfs::Read(uint32_t handle, uint64_t offset, void* buffer, uint32_t length, uint32_t* transferred) {
  *transferred = 0;
  auto f = Find(handle);
  if (!f) return kStatusInvalidHandle;
  if (f->directory) return kStatusNotADirectory;
  const bool own_position = offset == kCurrentPosition;
  const uint64_t at = own_position ? f->position.load() : offset;
  ++reads_;
  if (length == 0) return kStatusSuccess;
  if (at >= f->size) {
    ++eof_reads_;
    return kStatusEndOfFile;
  }
  uint32_t done = 0;
  while (done < length) {
    const ssize_t n = ::pread(f->fd, static_cast<uint8_t*>(buffer) + done, length - done, static_cast<off_t>(at + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return kStatusIoDeviceError;
    }
    if (n == 0) break;
    done += static_cast<uint32_t>(n);
  }
  if (own_position) f->position.store(at + done);
  *transferred = done;
  bytes_ += done;
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++f->record->reads;
    f->record->bytes += done;
  }
  return kStatusSuccess;
}

NtStatus Vfs::Info(uint32_t handle, FileInfo* info) {
  auto f = Find(handle);
  if (!f) return kStatusInvalidHandle;
  info->size = f->size;
  info->position = f->position.load();
  info->directory = f->directory;
  return kStatusSuccess;
}

NtStatus Vfs::SetPosition(uint32_t handle, uint64_t position) {
  auto f = Find(handle);
  if (!f) return kStatusInvalidHandle;
  f->position.store(position);
  return kStatusSuccess;
}

NtStatus Vfs::Close(uint32_t handle) {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = handles_.find(handle);
  if (it == handles_.end()) return kStatusInvalidHandle;
  handles_.erase(it);
  ++closes_;
  return kStatusSuccess;
}

VfsStats Vfs::Stats() const {
  VfsStats s;
  s.opens = opens_;
  s.open_failures = open_failures_;
  s.reads = reads_;
  s.bytes = bytes_;
  s.end_of_file_reads = eof_reads_;
  s.closes = closes_;
  return s;
}

std::vector<FileRecord> Vfs::Files() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<FileRecord> out;
  for (const auto& r : records_) out.push_back(*r);
  return out;
}

}  // namespace gh2k
