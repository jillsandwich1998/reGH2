#include "gh2k/ark.h"

#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace gh2k {

namespace {

bool KnownVersion(int32_t v) { return v == 2 || v == 3 || v == 4 || v == 5 || v == 6 || v == 7 || v == 9 || v == 10; }

int32_t Wrap32(int64_t x) { return static_cast<int32_t>(static_cast<uint32_t>(x & 0xFFFFFFFFll)); }

// One step of the header keystream (signed 32-bit arithmetic, C-style truncating division).
int32_t NextKey(int32_t data) {
  const int64_t q = static_cast<int64_t>(data) / 0x1F31D;
  int32_t value = Wrap32((static_cast<int64_t>(data) - q * 0x1F31D) * 0x41A7 - q * 0xB14);
  if (value <= 0) value = Wrap32(static_cast<int64_t>(value) + 0x7FFFFFFF);
  return value;
}

struct Reader {
  const std::vector<uint8_t>& d;
  size_t p;
  bool bad = false;
  uint32_t U32() {
    if (p + 4 > d.size()) { bad = true; return 0; }
    uint32_t v;
    std::memcpy(&v, &d[p], 4);
    p += 4;
    return v;
  }
  int32_t I32() { return static_cast<int32_t>(U32()); }
  uint64_t I64() {
    if (p + 8 > d.size()) { bad = true; return 0; }
    uint64_t v;
    std::memcpy(&v, &d[p], 8);
    p += 8;
    return v;
  }
  std::string CStr() {
    size_t end = p;
    while (end < d.size() && d[end] != 0) ++end;
    if (end >= d.size()) { bad = true; return {}; }
    std::string s(reinterpret_cast<const char*>(&d[p]), end - p);
    p = end + 1;
    return s;
  }
  std::string LStr() {
    const uint32_t n = U32();
    if (p + n > d.size()) { bad = true; return {}; }
    std::string s(reinterpret_cast<const char*>(&d[p]), n);
    p += n;
    return s;
  }
};

std::string KeyOf(std::string_view path) {
  std::string out(path);
  for (char& c : out) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

}  // namespace

std::unique_ptr<ArkArchive> ArkArchive::Open(const fs::path& header, std::string* error) {
  auto fail = [&](const char* why) -> std::unique_ptr<ArkArchive> {
    if (error) *error = why;
    return nullptr;
  };
  std::ifstream in(header, std::ios::binary);
  if (!in) return fail("cannot open the header");
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.size() < 16) return fail("header too short");

  std::unique_ptr<ArkArchive> ark(new ArkArchive());
  ark->header_ = header;
  int32_t version;
  std::memcpy(&version, &data[0], 4);
  size_t start = 4;
  if (!KnownVersion(version)) {
    ark->encrypted_ = true;
    int32_t key = version;
    for (size_t i = 4; i < data.size(); ++i) {
      key = NextKey(key);
      data[i] ^= static_cast<uint8_t>(key & 0xFF);
    }
    std::memcpy(&version, &data[4], 4);
    if (!KnownVersion(version)) {
      version = static_cast<int32_t>(~static_cast<uint32_t>(version));
      if (!KnownVersion(version)) return fail("unsupported ARK version after decryption");
      for (size_t i = 8; i < data.size(); ++i) data[i] ^= 0xFF;
    }
    start = 8;
  }
  if (version > 7) return fail("ARK version not supported (3-7 are)");
  ark->version_ = static_cast<uint32_t>(version);

  Reader r{data, start};
  if (version >= 6) r.p += static_cast<size_t>(r.U32()) << 4;
  (void)r.U32();   // part count
  const uint32_t size_count = r.U32();
  const size_t sizes_at = r.p;
  std::vector<uint64_t> sizes;
  bool broken_v4 = false;
  if (version != 4) {
    for (uint32_t i = 0; i < size_count && !r.bad; ++i) sizes.push_back(r.U32());
  } else {
    for (uint32_t i = 0; i < size_count && !r.bad; ++i) sizes.push_back(r.I64());
    if (!sizes.empty() && sizes.back() > 0xFFFFFFFFull) {   // a v3/v5 hybrid some builds ship
      broken_v4 = true;
      r.p = sizes_at;
      sizes.clear();
      for (uint32_t i = 0; i < size_count && !r.bad; ++i) sizes.push_back(r.U32());
    }
  }
  if (version >= 5 || broken_v4) {
    for (uint32_t n = r.U32(); n > 0 && !r.bad; --n) r.LStr();
  }
  if (version >= 6) r.p += static_cast<size_t>(r.U32()) << 2;
  if (version >= 7) {
    for (uint32_t n = r.U32(); n > 0 && !r.bad; --n)
      for (uint32_t m = r.U32(); m > 0 && !r.bad; --m) r.LStr();
  }
  const uint32_t blob_size = r.U32();
  const size_t blob_start = r.p;
  std::unordered_map<uint32_t, std::string> strings;
  while (!r.bad && r.p - blob_start < blob_size) {
    const uint32_t off = static_cast<uint32_t>(r.p - blob_start);
    strings[off] = r.CStr();
  }
  std::vector<int32_t> index;
  for (uint32_t n = r.U32(); n > 0 && !r.bad; --n) index.push_back(r.I32());
  const uint32_t count = r.U32();
  if (r.bad) return fail("header truncated");
  const bool wide = version >= 4 && !broken_v4;
  ark->part_sizes_ = sizes;
  ark->entries_.reserve(count);
  auto str = [&](int32_t i) -> const std::string* {
    if (i < 0 || static_cast<size_t>(i) >= index.size()) return nullptr;
    auto it = strings.find(static_cast<uint32_t>(index[static_cast<size_t>(i)]));
    return it == strings.end() ? nullptr : &it->second;
  };
  for (uint32_t k = 0; k < count; ++k) {
    ArkEntry e;
    e.offset = wide ? r.I64() : r.U32();
    const std::string* name = str(r.I32());
    const std::string* dir = str(r.I32());
    e.size = r.U32();
    e.inflated = r.U32();
    if (r.bad || !name || !dir) return fail("entry table corrupt");
    e.path = dir->empty() ? *name : (*dir + "/" + *name);
    uint64_t base = 0;
    e.part = 0;
    while (e.part + 1 < sizes.size() && e.offset >= base + sizes[e.part]) {
      base += sizes[e.part];
      ++e.part;
    }
    e.part_offset = e.offset - base;
    ark->by_path_.emplace(KeyOf(e.path), ark->entries_.size());
    ark->entries_.push_back(std::move(e));
  }
  return ark;
}

const ArkEntry* ArkArchive::Find(std::string_view path) const {
  auto it = by_path_.find(KeyOf(path));
  return it == by_path_.end() ? nullptr : &entries_[it->second];
}

fs::path ArkArchive::PartPath(uint32_t part) const {
  return header_.parent_path() / (header_.stem().string() + "_" + std::to_string(part) + ".ark");
}

bool ArkArchive::ReadStored(const ArkEntry& e, uint64_t offset, void* buffer, uint32_t length, std::string* error) const {
  const int fd = ::open(PartPath(e.part).c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (error) *error = "cannot open the part";
    return false;
  }
  uint32_t done = 0;
  while (done < length) {
    const ssize_t n = ::pread(fd, static_cast<uint8_t*>(buffer) + done, length - done, static_cast<off_t>(e.part_offset + offset + done));
    if (n <= 0) break;
    done += static_cast<uint32_t>(n);
  }
  ::close(fd);
  if (done != length) {
    if (error) *error = "short read";
    return false;
  }
  return true;
}

bool ArkArchive::Read(const ArkEntry& e, std::vector<uint8_t>* out, std::string* error) const {
  std::vector<uint8_t> stored(e.size);
  if (!ReadStored(e, 0, stored.data(), e.size, error)) return false;
  if (!e.inflated || e.inflated == e.size) {
    *out = std::move(stored);
    return true;
  }
  out->assign(e.inflated, 0);
  z_stream z{};
  const bool zlib_header = stored.size() >= 2 && stored[0] == 0x78 && (stored[1] == 0x9C || stored[1] == 0x01 || stored[1] == 0xDA);
  if (inflateInit2(&z, zlib_header ? 15 : -15) != Z_OK) {
    if (error) *error = "zlib init";
    return false;
  }
  z.next_in = stored.data();
  z.avail_in = static_cast<uInt>(stored.size());
  z.next_out = out->data();
  z.avail_out = static_cast<uInt>(out->size());
  const int rc = inflate(&z, Z_FINISH);
  inflateEnd(&z);
  if (rc != Z_STREAM_END || z.total_out != e.inflated) {
    if (error) *error = "corrupt deflate stream";
    return false;
  }
  return true;
}

}  // namespace gh2k
