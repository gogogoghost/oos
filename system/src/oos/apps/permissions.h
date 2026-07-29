#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oos::apps {

enum class DeviceServicePermission : uint32_t {
  AudioCapture = 1U << 0,
  Camera = 1U << 1,
  Power = 1U << 2,
  Wifi = 1U << 3,
  Bluetooth = 1U << 4,
  Modem = 1U << 5,
  DeviceStorage = 1U << 6,
};

struct DataStoreGrant {
  std::string name;
  bool writable = false;
};

constexpr uint32_t permissionBit(DeviceServicePermission permission) {
  return static_cast<uint32_t>(permission);
}

inline uint32_t
deviceServicePermissionMask(const std::vector<std::string> &permissions) {
  uint32_t mask = 0;
  for (const std::string &permission : permissions) {
    if (permission == "audio-capture") {
      mask |= permissionBit(DeviceServicePermission::AudioCapture);
    } else if (permission == "camera") {
      mask |= permissionBit(DeviceServicePermission::Camera);
    } else if (permission == "power") {
      mask |= permissionBit(DeviceServicePermission::Power);
    } else if (permission == "wifi-manage" || permission == "wifi") {
      mask |= permissionBit(DeviceServicePermission::Wifi);
    } else if (permission == "bluetooth") {
      mask |= permissionBit(DeviceServicePermission::Bluetooth);
    } else if (permission == "mobileconnection" ||
               permission == "mobilenetwork") {
      mask |= permissionBit(DeviceServicePermission::Modem);
    } else if (permission.compare(0, sizeof("device-storage:") - 1,
                                  "device-storage:") == 0) {
      mask |= permissionBit(DeviceServicePermission::DeviceStorage);
    }
  }
  return mask;
}

constexpr bool hasDeviceServicePermission(
    uint32_t mask, DeviceServicePermission permission) {
  return (mask & permissionBit(permission)) != 0;
}

inline std::vector<DataStoreGrant>
ownedDataStoreGrants(const std::vector<std::string> &permissions) {
  constexpr const char read_only[] = "datastore-owned:readonly:";
  constexpr const char read_write[] = "datastore-owned:readwrite:";
  std::vector<DataStoreGrant> grants;
  for (const std::string &permission : permissions) {
    const char *prefix = nullptr;
    size_t prefix_size = 0;
    if (permission.compare(0, sizeof(read_only) - 1, read_only) == 0) {
      prefix = read_only;
      prefix_size = sizeof(read_only) - 1;
    } else if (permission.compare(0, sizeof(read_write) - 1,
                                  read_write) == 0) {
      prefix = read_write;
      prefix_size = sizeof(read_write) - 1;
    }
    if (prefix && permission.size() > prefix_size)
      grants.push_back({permission.substr(prefix_size), true});
  }
  return grants;
}

} // namespace oos::apps
