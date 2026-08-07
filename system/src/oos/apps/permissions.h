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
  DeviceStorageRead = 1U << 6,
  DeviceStorageWrite = 1U << 7,
  DeviceStorageCreate = 1U << 8,
  System = 1U << 9,
  ModemIdentity = 1U << 10,
  ModemRadioControl = 1U << 11,
  AppsLaunch = 1U << 12,
  AppsManagement = 1U << 13,
  SystemUi = 1U << 14,
  SystemSettings = 1U << 15,
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
    } else if (permission == "device-storage" ||
               permission == "device-storage:read") {
      mask |= permissionBit(DeviceServicePermission::DeviceStorageRead);
    } else if (permission == "device-storage:write") {
      mask |= permissionBit(DeviceServicePermission::DeviceStorageWrite);
    } else if (permission == "device-storage:create") {
      mask |= permissionBit(DeviceServicePermission::DeviceStorageCreate);
    } else if (permission == "system") {
      mask |= permissionBit(DeviceServicePermission::System);
    } else if (permission == "mobileconnection:identity") {
      mask |= permissionBit(DeviceServicePermission::ModemIdentity);
    } else if (permission == "mobileconnection:radio-control") {
      mask |= permissionBit(DeviceServicePermission::ModemRadioControl);
    } else if (permission == "apps-launch") {
      mask |= permissionBit(DeviceServicePermission::AppsLaunch);
    } else if (permission == "apps-management") {
      mask |= permissionBit(DeviceServicePermission::AppsManagement);
    } else if (permission == "system-ui") {
      mask |= permissionBit(DeviceServicePermission::SystemUi);
    } else if (permission == "system-settings") {
      mask |= permissionBit(DeviceServicePermission::SystemSettings);
    }
  }
  return mask;
}

constexpr bool hasDeviceServicePermission(uint32_t mask,
                                          DeviceServicePermission permission) {
  return (mask & permissionBit(permission)) != 0;
}

} // namespace oos::apps
