// Minimal zip writer for the problem report: stores the files without compression (the screenshot is a PNG already), no zlib needed.
#pragma once

#include <string>
#include <utility>
#include <vector>

// `files` are (name inside the zip, path on disk). Returns false if a file cannot be read or the zip cannot be written.
bool Gh2WriteZip(const std::string& zip_path, const std::vector<std::pair<std::string, std::string>>& files);
