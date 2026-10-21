#include "zip_writer.h"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>

#include "png_writer.h"

namespace {

void P16(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x & 0xFF);
  v.push_back((x >> 8) & 0xFF);
}
void P32(std::vector<uint8_t>& v, uint32_t x) {
  P16(v, x & 0xFFFF);
  P16(v, x >> 16);
}

}  // namespace

bool Gh2WriteZip(const std::string& zip_path, const std::vector<std::pair<std::string, std::string>>& files) {
  const std::time_t now = std::time(nullptr);
  const std::tm* t = std::localtime(&now);
  const uint32_t dos_time = (t->tm_hour << 11) | (t->tm_min << 5) | (t->tm_sec / 2);
  const uint32_t dos_date = ((t->tm_year - 80) << 9) | ((t->tm_mon + 1) << 5) | t->tm_mday;

  std::vector<uint8_t> out, central;
  for (const auto& [name, path] : files) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const uint32_t crc = Gh2Crc32(data.data(), data.size());
    const uint32_t offset = static_cast<uint32_t>(out.size());
    // local file header
    P32(out, 0x04034B50); P16(out, 20); P16(out, 0); P16(out, 0);  // signature, version, flags, method 0 = stored
    P16(out, dos_time); P16(out, dos_date); P32(out, crc);
    P32(out, static_cast<uint32_t>(data.size())); P32(out, static_cast<uint32_t>(data.size()));
    P16(out, static_cast<uint32_t>(name.size())); P16(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), data.begin(), data.end());
    // central directory entry
    P32(central, 0x02014B50); P16(central, 20); P16(central, 20); P16(central, 0); P16(central, 0);
    P16(central, dos_time); P16(central, dos_date); P32(central, crc);
    P32(central, static_cast<uint32_t>(data.size())); P32(central, static_cast<uint32_t>(data.size()));
    P16(central, static_cast<uint32_t>(name.size())); P16(central, 0); P16(central, 0);  // name, extra, comment lengths
    P16(central, 0); P16(central, 0); P32(central, 0); P32(central, offset);               // disk, internal and external attributes, offset
    central.insert(central.end(), name.begin(), name.end());
  }
  const uint32_t central_offset = static_cast<uint32_t>(out.size());
  out.insert(out.end(), central.begin(), central.end());
  P32(out, 0x06054B50); P16(out, 0); P16(out, 0);  // end of central directory: this disk, disk with the directory
  P16(out, static_cast<uint32_t>(files.size())); P16(out, static_cast<uint32_t>(files.size()));
  P32(out, static_cast<uint32_t>(central.size())); P32(out, central_offset); P16(out, 0);
  FILE* f = fopen(zip_path.c_str(), "wb");
  if (!f) return false;
  const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
  fclose(f);
  return ok;
}
