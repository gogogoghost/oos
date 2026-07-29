#include "oos/device/service_provider.h"

#include "oos/device/services.h"

#include <utility>

namespace oos::device {

struct ServiceProvider::Impl {
  explicit Impl(const Device &device) : device(device) {}

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
  return impl_->ensureCamera() && impl_->finish(
                                      impl_->camera->setTorch(camera_id, enabled),
                                      *impl_->camera);
}

bool ServiceProvider::captureJpeg(const std::string &camera_id,
                                  const std::string &path,
                                  hardware::PhotoResult &result, int max_width,
                                  int max_height, bool flash, int timeout_ms) {
  return impl_->ensureCamera() &&
         impl_->finish(impl_->camera->captureJpeg(
                           camera_id, path, result, max_width, max_height, flash,
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
  return impl_->ensurePower() && impl_->finish(
                                      impl_->power->setInteractive(interactive),
                                      *impl_->power);
}

bool ServiceProvider::acquireWakeLock(const std::string &name) {
  return impl_->ensurePower() && impl_->finish(
                                      impl_->power->acquireWakeLock(name),
                                      *impl_->power);
}

bool ServiceProvider::releaseWakeLock(const std::string &name) {
  return impl_->ensurePower() && impl_->finish(
                                      impl_->power->releaseWakeLock(name),
                                      *impl_->power);
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
  return impl_->ensureBluetooth() && impl_->finish(
                                          impl_->bluetooth->disable(timeout_ms),
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
  return impl_->ensureBluetooth() && impl_->finish(
                                          impl_->bluetooth->pair(address,
                                                                 transport),
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
         impl_->finish(
             impl_->bluetooth->profileConnectionCycle(address, profile,
                                                       hold_ms),
             *impl_->bluetooth);
}

bool ServiceProvider::bluetoothLeConnectionCycle(const std::string &address,
                                                 int hold_ms,
                                                 int timeout_ms) {
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

bool ServiceProvider::setRadioPower(bool enabled,
                                    modem::ModemRequestStatus &status,
                                    int timeout_ms) {
  return impl_->ensureModem() &&
         impl_->finish(
             impl_->modem->setRadioPower(enabled, status, timeout_ms),
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
