#include "oos/device/service_provider.h"

#include "oos/device/services.h"
#include "oos/platform/android_properties.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace oos::device {

struct ServiceProvider::Impl {
  explicit Impl(const Device &device) : device(device) {}

  ~Impl() {
    if (legacy_wifi_hal)
      dlclose(legacy_wifi_hal);
  }

  bool ensureAudio() {
    if (!audio)
      audio = std::make_unique<hardware::AudioManager>();
    return true;
  }

  bool ensureCodec() {
    if (!codec)
      codec = std::make_unique<hardware::CodecManager>();
    return true;
  }

  bool ensureCamera() {
    if (camera)
      return true;
    camera = std::make_unique<hardware::CameraManager>();
    if (initializeService(device, *camera))
      return true;
    error = camera->lastError();
    camera.reset();
    return false;
  }

  bool ensurePower() {
    if (power)
      return true;
    power = std::make_unique<hardware::PowerManager>();
    if (initializeService(device, *power))
      return true;
    error = power->lastError();
    power.reset();
    return false;
  }

  bool ensureVibrator() {
    if (vibrator)
      return true;
    vibrator = std::make_unique<hardware::VibratorManager>();
    if (initializeService(device, *vibrator))
      return true;
    error = vibrator->lastError();
    vibrator.reset();
    return false;
  }

  bool ensureWifi() {
    if (wifi)
      return true;
    wifi = std::make_unique<network::WifiManager>(
        device.services().wifi_control_socket);
    if (initializeService(device, *wifi))
      return true;
    error = wifi->lastError();
    wifi.reset();
    return false;
  }

  bool wifiEndpointPresent() const {
    return access(device.services().wifi_control_socket, F_OK) == 0;
  }

  bool wifiSupplicantRunning() const {
    char state[PROPERTY_VALUE_MAX] = {};
    property_get("init.svc.wpa_supplicant", state, "");
    return std::strcmp(state, "running") == 0;
  }

  bool waitForWifiState(bool enabled, int timeout_seconds) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(timeout_seconds);
    do {
      const bool ready = wifiSupplicantRunning() && wifiEndpointPresent();
      if (ready == enabled)
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
    error = enabled ? "Wi-Fi supplicant did not start"
                    : "Wi-Fi supplicant did not stop";
    return false;
  }

  template <typename Function>
  bool resolveLegacyWifiFunction(Function &function, const char *name) {
    function = reinterpret_cast<Function>(dlsym(legacy_wifi_hal, name));
    if (function)
      return true;
    error = std::string("resolve ") + name + ": " + dlerror();
    return false;
  }

  bool ensureLegacyWifiHal() {
    if (legacy_wifi_hal)
      return true;
    legacy_wifi_hal =
        dlopen("/system/lib/libhardware_legacy.so", RTLD_NOW | RTLD_LOCAL);
    if (!legacy_wifi_hal) {
      error = std::string("load libhardware_legacy.so: ") + dlerror();
      return false;
    }
    if (resolveLegacyWifiFunction(legacy_wifi_driver_loaded,
                                  "is_wifi_driver_loaded") &&
        resolveLegacyWifiFunction(legacy_wifi_load_driver,
                                  "wifi_load_driver") &&
        resolveLegacyWifiFunction(legacy_wifi_unload_driver,
                                  "wifi_unload_driver") &&
        resolveLegacyWifiFunction(legacy_wifi_start_supplicant,
                                  "wifi_start_supplicant") &&
        resolveLegacyWifiFunction(legacy_wifi_stop_supplicant,
                                  "wifi_stop_supplicant"))
      return true;
    dlclose(legacy_wifi_hal);
    legacy_wifi_hal = nullptr;
    return false;
  }

  bool setLegacyWifiEnabled(bool enabled) {
    if (!ensureLegacyWifiHal())
      return false;
    if (enabled) {
      if (!legacy_wifi_driver_loaded() && legacy_wifi_load_driver() != 0) {
        error = "wifi_load_driver failed";
        return false;
      }
      if (!wifiSupplicantRunning() && legacy_wifi_start_supplicant(0) != 0) {
        error = "wifi_start_supplicant failed";
        return false;
      }
      if (!waitForWifiState(true, 12))
        return false;
    } else {
      wifi.reset();
      property_set("ctl.stop", "dhcpcd_wlan0");
      if (wifiSupplicantRunning() && legacy_wifi_stop_supplicant(0) != 0) {
        error = "wifi_stop_supplicant failed";
        return false;
      }
      if (!waitForWifiState(false, 8))
        return false;
      if (legacy_wifi_driver_loaded() && legacy_wifi_unload_driver() != 0) {
        error = "wifi_unload_driver failed";
        return false;
      }
    }
    error.clear();
    return true;
  }

  bool runWifiServiceCommand(bool enabled) {
    const pid_t child = fork();
    if (child < 0) {
      error = "fork svc wifi: " + std::string(std::strerror(errno));
      return false;
    }
    if (child == 0) {
      if (device.descriptor().android_api >= 26) {
        execl("/system/bin/cmd", "cmd", "wifi", "set-wifi-enabled",
              enabled ? "enabled" : "disabled", static_cast<char *>(nullptr));
      } else {
        execl("/system/bin/svc", "svc", "wifi", enabled ? "enable" : "disable",
              static_cast<char *>(nullptr));
      }
      _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
      if (errno == EINTR)
        continue;
      error = "wait for svc wifi: " + std::string(std::strerror(errno));
      return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      error = "svc wifi command failed";
      return false;
    }
    return true;
  }

  bool ensureIp() {
    if (!ip)
      ip = std::make_unique<network::IpManager>("wlan0");
    return true;
  }

  bool ensureBluetooth() {
    if (bluetooth)
      return true;
    bluetooth = std::make_unique<network::BluetoothManager>();
    if (initializeService(device, *bluetooth))
      return true;
    error = bluetooth->lastError();
    bluetooth.reset();
    return false;
  }

  bool ensureModem() {
    if (modem)
      return true;
    modem = std::make_unique<oos::modem::ModemManager>();
    if (initializeService(device, *modem))
      return true;
    error = modem->lastError();
    modem.reset();
    return false;
  }

  template <typename Manager>
  bool finish(bool success, const Manager &manager) {
    if (success) {
      error.clear();
      return true;
    }
    error = manager.lastError();
    return false;
  }

  const Device &device;
  std::unique_ptr<hardware::AudioManager> audio;
  std::unique_ptr<hardware::CodecManager> codec;
  std::unique_ptr<hardware::CameraManager> camera;
  std::unique_ptr<hardware::PowerManager> power;
  std::unique_ptr<hardware::VibratorManager> vibrator;
  std::unique_ptr<network::WifiManager> wifi;
  std::unique_ptr<network::IpManager> ip;
  std::unique_ptr<network::BluetoothManager> bluetooth;
  std::unique_ptr<oos::modem::ModemManager> modem;
  void *legacy_wifi_hal = nullptr;
  int (*legacy_wifi_driver_loaded)() = nullptr;
  int (*legacy_wifi_load_driver)() = nullptr;
  int (*legacy_wifi_unload_driver)() = nullptr;
  int (*legacy_wifi_start_supplicant)(int) = nullptr;
  int (*legacy_wifi_stop_supplicant)(int) = nullptr;
  std::string error;
};

ServiceProvider::ServiceProvider(const Device &device)
    : impl_(std::make_unique<Impl>(device)) {}

ServiceProvider::~ServiceProvider() = default;

bool ServiceProvider::playTone(double frequency_hz, int duration_ms,
                               float volume, hardware::AudioUsage usage,
                               hardware::AudioStreamInfo &info) {
  return impl_->ensureAudio() &&
         impl_->finish(impl_->audio->playTone(frequency_hz, duration_ms, volume,
                                              usage, info),
                       *impl_->audio);
}

bool ServiceProvider::recordWav(const std::string &path, int duration_ms,
                                hardware::RecordingResult &result) {
  return impl_->ensureAudio() &&
         impl_->finish(impl_->audio->recordWav(path, duration_ms, result),
                       *impl_->audio);
}

bool ServiceProvider::enumerateCameras(
    std::vector<hardware::CameraInfo> &cameras) {
  return impl_->ensureCamera() &&
         impl_->finish(impl_->camera->enumerate(cameras), *impl_->camera);
}

bool ServiceProvider::setTorch(const std::string &camera_id, bool enabled) {
  return impl_->ensureCamera() &&
         impl_->finish(impl_->camera->setTorch(camera_id, enabled),
                       *impl_->camera);
}

bool ServiceProvider::captureJpeg(const std::string &camera_id,
                                  const std::string &path,
                                  hardware::PhotoResult &result, int max_width,
                                  int max_height, bool flash, int timeout_ms) {
  return impl_->ensureCamera() &&
         impl_->finish(impl_->camera->captureJpeg(camera_id, path, result,
                                                  max_width, max_height, flash,
                                                  timeout_ms),
                       *impl_->camera);
}

bool ServiceProvider::queryBattery(hardware::BatterySnapshot &snapshot) {
  return impl_->ensurePower() &&
         impl_->finish(impl_->power->queryBattery(snapshot), *impl_->power);
}

int ServiceProvider::waitForBatteryEvent(int timeout_ms,
                                         hardware::BatterySnapshot &snapshot) {
  if (!impl_->ensurePower())
    return -1;
  const int result = impl_->power->waitForBatteryEvent(timeout_ms, snapshot);
  if (result < 0)
    impl_->error = impl_->power->lastError();
  else
    impl_->error.clear();
  return result;
}

bool ServiceProvider::setInteractive(bool interactive) {
  return impl_->ensurePower() &&
         impl_->finish(impl_->power->setInteractive(interactive),
                       *impl_->power);
}

bool ServiceProvider::acquireWakeLock(const std::string &name) {
  return impl_->ensurePower() &&
         impl_->finish(impl_->power->acquireWakeLock(name), *impl_->power);
}

bool ServiceProvider::releaseWakeLock(const std::string &name) {
  return impl_->ensurePower() &&
         impl_->finish(impl_->power->releaseWakeLock(name), *impl_->power);
}

bool ServiceProvider::enableAutoSuspend() {
  return impl_->ensurePower() &&
         impl_->finish(impl_->power->enableAutoSuspend(), *impl_->power);
}

bool ServiceProvider::disableAutoSuspend() {
  return impl_->ensurePower() &&
         impl_->finish(impl_->power->disableAutoSuspend(), *impl_->power);
}

bool ServiceProvider::scheduleRtcWake(int delay_seconds) {
  return impl_->ensurePower() &&
         impl_->finish(impl_->power->scheduleRtcWake(delay_seconds),
                       *impl_->power);
}

bool ServiceProvider::clearRtcWake() {
  return impl_->ensurePower() &&
         impl_->finish(impl_->power->clearRtcWake(), *impl_->power);
}

bool ServiceProvider::suspend(int graceful_timeout_ms) {
  return impl_->ensurePower() &&
         impl_->finish(impl_->power->suspend(graceful_timeout_ms),
                       *impl_->power);
}

hardware::FlipState ServiceProvider::queryFlipState() {
  if (!impl_->ensurePower())
    return hardware::FlipState::Unknown;
  return impl_->power->queryFlipState(impl_->device.services().input_directory);
}

bool ServiceProvider::vibrate(uint32_t duration_ms) {
  return impl_->ensureVibrator() &&
         impl_->finish(impl_->vibrator->vibrate(duration_ms), *impl_->vibrator);
}

bool ServiceProvider::stopVibration() {
  return impl_->ensureVibrator() &&
         impl_->finish(impl_->vibrator->stop(), *impl_->vibrator);
}

bool ServiceProvider::supportsAmplitudeControl() {
  return impl_->ensureVibrator() && impl_->vibrator->supportsAmplitudeControl();
}

bool ServiceProvider::setVibrationAmplitude(uint8_t amplitude) {
  return impl_->ensureVibrator() &&
         impl_->finish(impl_->vibrator->setAmplitude(amplitude),
                       *impl_->vibrator);
}

bool ServiceProvider::wifiStatus(network::WifiStatus &status) {
  return impl_->ensureWifi() &&
         impl_->finish(impl_->wifi->status(status), *impl_->wifi);
}

bool ServiceProvider::wifiEnabled(bool &enabled) {
  enabled =
      impl_->device.services().wifi_lifecycle == WifiLifecycle::LegacyHardware
          ? impl_->wifiSupplicantRunning() && impl_->wifiEndpointPresent()
          : impl_->wifiEndpointPresent();
  impl_->error.clear();
  return true;
}

bool ServiceProvider::wifiSetEnabled(bool enabled) {
  if (impl_->device.services().wifi_lifecycle == WifiLifecycle::LegacyHardware)
    return impl_->setLegacyWifiEnabled(enabled);
  if (impl_->wifiEndpointPresent() == enabled) {
    impl_->error.clear();
    return true;
  }
  if (!enabled)
    impl_->wifi.reset();
  if (!impl_->runWifiServiceCommand(enabled))
    return false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(enabled ? 12 : 8);
  while (std::chrono::steady_clock::now() < deadline) {
    if (impl_->wifiEndpointPresent() == enabled) {
      impl_->error.clear();
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  impl_->error = enabled ? "Wi-Fi supplicant did not start"
                         : "Wi-Fi supplicant did not stop";
  return false;
}

bool ServiceProvider::wifiScan(std::vector<network::WifiAccessPoint> &results,
                               int wait_ms) {
  return impl_->ensureWifi() &&
         impl_->finish(impl_->wifi->scan(results, wait_ms), *impl_->wifi);
}

bool ServiceProvider::wifiListNetworks(
    std::vector<network::WifiNetwork> &networks) {
  return impl_->ensureWifi() &&
         impl_->finish(impl_->wifi->listNetworks(networks), *impl_->wifi);
}

bool ServiceProvider::wifiConnect(const std::string &ssid,
                                  network::WifiSecurity security,
                                  const std::string &credential,
                                  int &network_id) {
  return impl_->ensureWifi() &&
         impl_->finish(
             impl_->wifi->connect(ssid, security, credential, network_id),
             *impl_->wifi);
}

bool ServiceProvider::wifiSelect(int network_id) {
  return impl_->ensureWifi() &&
         impl_->finish(impl_->wifi->select(network_id), *impl_->wifi);
}

bool ServiceProvider::wifiDisconnect() {
  return impl_->ensureWifi() &&
         impl_->finish(impl_->wifi->disconnect(), *impl_->wifi);
}

bool ServiceProvider::wifiReconnect() {
  return impl_->ensureWifi() &&
         impl_->finish(impl_->wifi->reconnect(), *impl_->wifi);
}

bool ServiceProvider::wifiForget(int network_id) {
  return impl_->ensureWifi() &&
         impl_->finish(impl_->wifi->forget(network_id), *impl_->wifi);
}

bool ServiceProvider::wifiSaveConfiguration() {
  return impl_->ensureWifi() &&
         impl_->finish(impl_->wifi->saveConfiguration(), *impl_->wifi);
}

bool ServiceProvider::ipStatus(network::IpConfiguration &configuration) {
  return impl_->ensureIp() &&
         impl_->finish(impl_->ip->status(configuration), *impl_->ip);
}

bool ServiceProvider::ipUseDhcp(int timeout_ms) {
  return impl_->ensureIp() &&
         impl_->finish(impl_->ip->useDhcp(timeout_ms), *impl_->ip);
}

bool ServiceProvider::ipUseStatic(
    const network::IpConfiguration &configuration) {
  return impl_->ensureIp() &&
         impl_->finish(impl_->ip->useStatic(configuration), *impl_->ip);
}

bool ServiceProvider::bluetoothEnable(int timeout_ms) {
  return impl_->ensureBluetooth() &&
         impl_->finish(impl_->bluetooth->enable(timeout_ms), *impl_->bluetooth);
}

bool ServiceProvider::bluetoothDisable(int timeout_ms) {
  return impl_->ensureBluetooth() &&
         impl_->finish(impl_->bluetooth->disable(timeout_ms),
                       *impl_->bluetooth);
}

bool ServiceProvider::bluetoothClassicScan(
    std::vector<network::BluetoothDevice> &devices, int duration_ms) {
  return impl_->ensureBluetooth() &&
         impl_->finish(impl_->bluetooth->classicScan(devices, duration_ms),
                       *impl_->bluetooth);
}

bool ServiceProvider::bluetoothLeScan(
    std::vector<network::BluetoothDevice> &devices, int duration_ms) {
  return impl_->ensureBluetooth() &&
         impl_->finish(impl_->bluetooth->leScan(devices, duration_ms),
                       *impl_->bluetooth);
}

bool ServiceProvider::bluetoothPair(const std::string &address,
                                    network::BluetoothTransport transport) {
  return impl_->ensureBluetooth() &&
         impl_->finish(impl_->bluetooth->pair(address, transport),
                       *impl_->bluetooth);
}

bool ServiceProvider::bluetoothUnpair(const std::string &address) {
  return impl_->ensureBluetooth() &&
         impl_->finish(impl_->bluetooth->unpair(address), *impl_->bluetooth);
}

bool ServiceProvider::bluetoothCancelPairing(const std::string &address) {
  return impl_->ensureBluetooth() &&
         impl_->finish(impl_->bluetooth->cancelPairing(address),
                       *impl_->bluetooth);
}

bool ServiceProvider::bluetoothProfileConnect(
    const std::string &address, network::BluetoothProfile profile) {
  return impl_->ensureBluetooth() &&
         impl_->finish(impl_->bluetooth->profileConnect(address, profile),
                       *impl_->bluetooth);
}

bool ServiceProvider::bluetoothProfileDisconnect(
    const std::string &address, network::BluetoothProfile profile) {
  return impl_->ensureBluetooth() &&
         impl_->finish(impl_->bluetooth->profileDisconnect(address, profile),
                       *impl_->bluetooth);
}

bool ServiceProvider::bluetoothProfileConnectionCycle(
    const std::string &address, network::BluetoothProfile profile,
    int hold_ms) {
  return impl_->ensureBluetooth() &&
         impl_->finish(impl_->bluetooth->profileConnectionCycle(
                           address, profile, hold_ms),
                       *impl_->bluetooth);
}

bool ServiceProvider::bluetoothLeConnectionCycle(const std::string &address,
                                                 int hold_ms, int timeout_ms) {
  return impl_->ensureBluetooth() &&
         impl_->finish(
             impl_->bluetooth->leConnectionCycle(address, hold_ms, timeout_ms),
             *impl_->bluetooth);
}

bool ServiceProvider::modemSnapshot(modem::ModemSnapshot &snapshot,
                                    int timeout_ms) {
  return impl_->ensureModem() &&
         impl_->finish(impl_->modem->querySnapshot(snapshot, timeout_ms),
                       *impl_->modem);
}

bool ServiceProvider::modemNetworkStatus(modem::NetworkStatus &status,
                                         int timeout_ms) {
  return impl_->ensureModem() &&
         impl_->finish(impl_->modem->queryNetworkStatus(status, timeout_ms),
                       *impl_->modem);
}

bool ServiceProvider::setRadioPower(bool enabled,
                                    modem::ModemRequestStatus &status,
                                    int timeout_ms) {
  return impl_->ensureModem() &&
         impl_->finish(impl_->modem->setRadioPower(enabled, status, timeout_ms),
                       *impl_->modem);
}

bool ServiceProvider::testH264RoundTrip(int width, int height, int frame_count,
                                        hardware::CodecResult &result,
                                        int timeout_ms) {
  return impl_->ensureCodec() &&
         impl_->finish(impl_->codec->testH264RoundTrip(
                           width, height, frame_count, result, timeout_ms),
                       *impl_->codec);
}

const std::string &ServiceProvider::lastError() const { return impl_->error; }

} // namespace oos::device
