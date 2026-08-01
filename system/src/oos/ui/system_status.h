#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace oos::device {
class Device;
}

namespace oos::ui {

struct SystemStatusSnapshot {
  uint64_t revision = 0;

  bool battery_available = false;
  int battery_percent = -1;
  bool charging = false;

  bool wifi_available = false;
  bool wifi_connected = false;

  bool cellular_available = false;
  bool cellular_registered = false;
  bool roaming = false;
  int signal_bars = 0;
  std::string radio_technology;
};

class SystemStatusSource {
public:
  virtual ~SystemStatusSource() = default;
  virtual SystemStatusSnapshot snapshot() const = 0;
};

// Queries potentially blocking platform services away from the input and
// rendering loop, then publishes immutable snapshots to SystemUI.
class DeviceStatusMonitor final : public SystemStatusSource {
public:
  explicit DeviceStatusMonitor(const device::Device &device);
  ~DeviceStatusMonitor() override;

  DeviceStatusMonitor(const DeviceStatusMonitor &) = delete;
  DeviceStatusMonitor &operator=(const DeviceStatusMonitor &) = delete;

  bool start();
  void stop();
  SystemStatusSnapshot snapshot() const override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::ui
