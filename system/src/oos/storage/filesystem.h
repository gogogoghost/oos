#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oos::storage {

bool ensureDirectory(const std::string &path, uint32_t mode,
                     std::string &error);
bool readFile(const std::string &path, std::vector<uint8_t> &output,
              size_t maximum_bytes, std::string &error);
bool writeFileAtomic(const std::string &path, const uint8_t *data, size_t size,
                     uint32_t mode, std::string &error);
bool copyFileAtomic(const std::string &source, const std::string &destination,
                    uint32_t mode, std::string &error);
std::string parentPath(const std::string &path);

} // namespace oos::storage
