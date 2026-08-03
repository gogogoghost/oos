#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oos::resources {

class PackageAssetService {
public:
  static constexpr uint32_t kMaximumOpenAssets = 32;
  static constexpr uint32_t kMaximumReadBytes = 1024 * 1024;

  explicit PackageAssetService(std::string root);
  ~PackageAssetService();

  PackageAssetService(const PackageAssetService &) = delete;
  PackageAssetService &operator=(const PackageAssetService &) = delete;

  bool open(const std::string &path, uint32_t &handle, uint64_t &size);
  bool readSize(uint32_t handle, uint64_t offset, uint32_t maximum_bytes,
                uint32_t &size);
  bool readInto(uint32_t handle, uint64_t offset, uint8_t *output,
                uint32_t capacity, uint32_t &bytes_read);
  bool read(uint32_t handle, uint64_t offset, uint32_t maximum_bytes,
            std::vector<uint8_t> &output);
  bool close(uint32_t handle);
  void closeAll();

  const std::string &lastError() const { return error_; }

private:
  struct Entry {
    uint32_t handle;
    int fd;
    uint64_t size;
  };

  Entry *find(uint32_t handle);

  std::string root_;
  std::vector<Entry> entries_;
  uint32_t next_handle_ = 1;
  std::string error_;
};

} // namespace oos::resources
