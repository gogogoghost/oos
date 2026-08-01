#include "oos/ui/system_status.h"

#include "oos/device/device.h"
#include "oos/device/service_provider.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace oos::ui {
namespace {

using Clock = std::chrono::steady_clock;

bool registered(int state) { return state == 1 || state == 5; }

int signalBars(const modem::SignalStatus &signal) {
  int strength = signal.lte_strength;
  if (strength < 0 || strength > 31)
    strength = signal.gsm_strength;
  if (strength < 0 || strength > 31)
    return 0;
  if (strength < 4)
    return 1;
  if (strength < 10)
    return 2;
  if (strength < 18)
    return 3;
  return 4;
}

std::string technologyLabel(int technology) {
  switch (technology) {
  case 1:  // GPRS
  case 2:  // EDGE
  case 16: // GSM
    return "2G";
  case 3:  // UMTS
  case 9:  // HSDPA
  case 10: // HSUPA
  case 11: // HSPA
  case 15: // HSPA+
  case 17: // TD-SCDMA
    return "3G";
  case 14: // LTE
  case 19: // LTE CA
    return "4G";
  default:
    return {};
  }
}

bool sameStatus(const SystemStatusSnapshot &left,
                const SystemStatusSnapshot &right) {
  return left.battery_available == right.battery_available &&
         left.battery_percent == right.battery_percent &&
         left.charging == right.charging &&
         left.wifi_available == right.wifi_available &&
         left.wifi_connected == right.wifi_connected &&
         left.cellular_available == right.cellular_available &&
         left.cellular_registered == right.cellular_registered &&
         left.roaming == right.roaming &&
         left.signal_bars == right.signal_bars &&
         left.radio_technology == right.radio_technology;
}

} // namespace

class DeviceStatusMonitor::Impl {
public:
  explicit Impl(const device::Device &device)
      : device(device), services(device) {}

  void publish(SystemStatusSnapshot next) {
    std::lock_guard<std::mutex> lock(mutex);
    if (sameStatus(status, next))
      return;
    next.revision = status.revision + 1;
    status = std::move(next);
  }

  void refreshBattery(SystemStatusSnapshot &next) {
    if (!device::isImplemented(device.capability(device::Feature::Battery)))
      return;
    hardware::BatterySnapshot battery;
    next.battery_available = services.queryBattery(battery);
    if (!next.battery_available)
      return;
    next.battery_percent = std::clamp(battery.capacity_percent, 0, 100);
    next.charging = battery.state == hardware::BatteryState::Charging ||
                    battery.state == hardware::BatteryState::Full;
  }

  void refreshWifi(SystemStatusSnapshot &next) {
    if (!device::isImplemented(device.capability(device::Feature::Wifi)))
      return;
    network::WifiStatus wifi;
    next.wifi_available = services.wifiStatus(wifi);
    next.wifi_connected =
        next.wifi_available && wifi.state == "COMPLETED" && !wifi.ssid.empty();
  }

  void refreshCellular(SystemStatusSnapshot &next) {
    if (!device::isImplemented(device.capability(device::Feature::Modem)))
      return;
    modem::NetworkStatus network;
    next.cellular_available = services.modemNetworkStatus(network, 1200);
    if (!next.cellular_available)
      return;
    const modem::RegistrationStatus &registration =
        registered(network.data_registration.state)
            ? network.data_registration
            : network.voice_registration;
    next.cellular_registered = registered(registration.state);
    next.roaming = registration.state == 5;
    next.signal_bars =
        next.cellular_registered ? signalBars(network.signal) : 0;
    next.radio_technology = next.cellular_registered
                                ? technologyLabel(registration.radio_technology)
                                : std::string();
  }

  void run() {
    auto next_battery = Clock::now();
    auto next_wifi = next_battery;
    auto next_cellular = next_battery;
    while (true) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (stop_requested)
          break;
      }

      SystemStatusSnapshot next = snapshot();
      const auto now = Clock::now();
      if (now >= next_battery) {
        refreshBattery(next);
        next_battery = now + std::chrono::seconds(30);
      }
      if (now >= next_wifi) {
        refreshWifi(next);
        next_wifi = now + std::chrono::seconds(5);
      }
      if (now >= next_cellular) {
        refreshCellular(next);
        next_cellular = now + std::chrono::seconds(15);
      }
      publish(std::move(next));

      const auto wake_at = std::min({next_battery, next_wifi, next_cellular});
      std::unique_lock<std::mutex> lock(mutex);
      wake.wait_until(lock, wake_at, [&] { return stop_requested; });
      if (stop_requested)
        break;
    }
  }

  SystemStatusSnapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return status;
  }

  const device::Device &device;
  device::ServiceProvider services;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  SystemStatusSnapshot status;
  bool running = false;
  bool stop_requested = false;
};

DeviceStatusMonitor::DeviceStatusMonitor(const device::Device &device)
    : impl_(std::make_unique<Impl>(device)) {}

DeviceStatusMonitor::~DeviceStatusMonitor() { stop(); }

bool DeviceStatusMonitor::start() {
  if (impl_->running)
    return true;
  impl_->stop_requested = false;
  impl_->worker = std::thread([this] { impl_->run(); });
  impl_->running = true;
  return true;
}

void DeviceStatusMonitor::stop() {
  if (!impl_ || !impl_->running)
    return;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stop_requested = true;
  }
  impl_->wake.notify_all();
  if (impl_->worker.joinable())
    impl_->worker.join();
  impl_->running = false;
}

SystemStatusSnapshot DeviceStatusMonitor::snapshot() const {
  return impl_->snapshot();
}

} // namespace oos::ui
