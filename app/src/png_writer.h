// Minimal PNG writer for the problem report's screenshot: 8-bit RGB, no compression (stored deflate blocks), so it needs no zlib.
#pragma once

#include <cstdint>
#include <string>

// `rgb` holds width * height * 3 bytes, rows top to bottom with no padding. Returns false if the file cannot be written.
bool Gh2WritePngRgb(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgb);
