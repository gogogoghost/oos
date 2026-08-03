#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oos::apps {

struct ZipEntry {
  std::string name;
  uint16_t compression_method = 0;
  uint32_t crc32 = 0;
  uint32_t compressed_size = 0;
  uint32_t uncompressed_size = 0;
  uint32_t local_header_offset = 0;
};

class ZipArchive {
public:
  ZipArchive() = default;
  ~ZipArchive();

  ZipArchive(const ZipArchive &) = delete;
  ZipArchive &operator=(const ZipArchive &) = delete;

  bool open(const char *path);
  void close();
  const ZipEntry *find(const char *name) const;
  bool read(const char *name, std::vector<uint8_t> &output,
            size_t maximum_bytes = 64 * 1024 * 1024) const;
  bool readEntry(const ZipEntry &entry, std::vector<uint8_t> &output,
                 size_t maximum_bytes = 64 * 1024 * 1024) const;

  const std::vector<ZipEntry> &entries() const { return entries_; }
  const std::string &lastError() const { return error_; }

private:
  bool fail(const std::string &message) const;

  int fd_ = -1;
  uint64_t file_size_ = 0;
  std::vector<ZipEntry> entries_;
  mutable std::string error_;
};

bool validPackagePath(const std::string &path);

} // namespace oos::apps
