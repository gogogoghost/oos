#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oos::storage {

enum class DeviceStorageVolume : uint32_t { Internal = 0, Removable = 1 };

enum class DeviceStorageWriteMode : uint32_t {
  Create = 0,
  Replace = 1,
  Append = 2,
};

struct DeviceStorageEntry {
  std::string path;
  uint64_t size = 0;
  int64_t last_modified_ms = 0;
};

// Shared implementation behind the WIT device-storage interface and the
// KaiOS DeviceStorage JavaScript adapter.
class DeviceStorageService {
public:
  DeviceStorageService(std::string internal_root = "/data/media/internal",
                       std::string removable_root = "/data/media/removable");

  bool list(DeviceStorageVolume volume,
            std::vector<DeviceStorageEntry> &entries);
  bool read(DeviceStorageVolume volume, const std::string &path,
            std::vector<uint8_t> &bytes);
  bool fileSize(DeviceStorageVolume volume, const std::string &path,
                uint64_t &size);
  bool readInto(DeviceStorageVolume volume, const std::string &path,
                uint8_t *bytes, size_t capacity, size_t &bytes_read);
  bool write(DeviceStorageVolume volume, const std::string &path,
             DeviceStorageWriteMode mode, const uint8_t *bytes, size_t size);
  bool remove(DeviceStorageVolume volume, const std::string &path,
              bool &removed);
  bool freeSpace(DeviceStorageVolume volume, uint64_t &bytes);
  bool usedSpace(DeviceStorageVolume volume, uint64_t &bytes);

  const std::string &lastError() const { return error_; }

private:
  const std::string &root(DeviceStorageVolume volume) const;
  bool space(DeviceStorageVolume volume, bool free, uint64_t &bytes);

  std::string internal_root_;
  std::string removable_root_;
  std::string error_;
};

} // namespace oos::storage
