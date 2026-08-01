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
  System = 1U << 7,
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
    } else if (permission == "system") {
      mask |= permissionBit(DeviceServicePermission::System);
    }
  }
  return mask;
}

constexpr bool hasDeviceServicePermission(uint32_t mask,
                                          DeviceServicePermission permission) {
  return (mask & permissionBit(permission)) != 0;
}

} // namespace oos::apps
