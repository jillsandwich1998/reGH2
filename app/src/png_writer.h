// Minimal PNG writer for the problem report's screenshot: 8-bit RGB, no compression (stored deflate blocks), so it needs no zlib.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// CRC-32 (the one PNG and zip use); pass the previous value to continue a running checksum.
uint32_t Gh2Crc32(const uint8_t* data, size_t n, uint32_t crc = 0);

// `rgb` holds width * height * 3 bytes, rows top to bottom with no padding. Returns false if the file cannot be written.
bool Gh2WritePngRgb(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgb);
