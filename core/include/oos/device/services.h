#pragma once

#include "oos/device/device.h"
#include "oos/hardware/camera_manager.h"
#include "oos/hardware/power_manager.h"
#include "oos/hardware/vibrator_manager.h"
#include "oos/modem/modem_manager.h"
#include "oos/network/bluetooth_manager.h"
#include "oos/network/ip_manager.h"
#include "oos/network/wifi_manager.h"

namespace oos::device {

// These helpers are the standard construction/initialization path for
// managers whose stock Android endpoint differs between target devices.
inline network::WifiManager createWifiManager(const Device &device) {
  return network::WifiManager(device.services().wifi_control_socket);
}

inline network::IpManager createIpManager(const Device &device) {
  (void)device;
  return network::IpManager("wlan0");
}

inline bool initializeService(const Device &device,
                              hardware::CameraManager &manager) {
  (void)device;
  return manager.initialize();
}

inline bool initializeService(const Device &device,
                              hardware::PowerManager &manager) {
  return manager.initialize(device.services().power_service);
}

inline bool initializeService(const Device &device,
                              hardware::VibratorManager &manager) {
  return manager.initialize(device.services().vibrator_service);
}

inline bool initializeService(const Device &device,
                              network::WifiManager &manager) {
  (void)device;
  return manager.initialize();
}

inline bool initializeService(const Device &device,
                              network::BluetoothManager &manager) {
  return manager.initialize(device.services().bluetooth_daemon);
}

inline bool initializeService(const Device &device,
                              modem::ModemManager &manager) {
  return manager.initialize(device.services().modem_service);
}

} // namespace oos::device
