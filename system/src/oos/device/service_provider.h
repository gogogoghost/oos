#pragma once

#include "oos/device/device.h"
#include "oos/hardware/audio_manager.h"
#include "oos/hardware/camera_manager.h"
#include "oos/hardware/codec_manager.h"
#include "oos/hardware/power_manager.h"
#include "oos/modem/modem_manager.h"
#include "oos/network/bluetooth_manager.h"
#include "oos/network/ip_manager.h"
#include "oos/network/wifi_manager.h"

#include <memory>
#include <string>
#include <vector>

namespace oos::device {

// One lazy service facade is shared by WPE adapters and WAMR imports. Device
// targets connect to their existing HAL managers; local supplies deterministic
// data through the same contract.
class ServiceProvider {
public:
  explicit ServiceProvider(const Device &device);
  ~ServiceProvider();

  ServiceProvider(const ServiceProvider &) = delete;
  ServiceProvider &operator=(const ServiceProvider &) = delete;

  bool playTone(double frequency_hz, int duration_ms, float volume,
                hardware::AudioUsage usage, hardware::AudioStreamInfo &info);
  bool recordWav(const std::string &path, int duration_ms,
                 hardware::RecordingResult &result);

  bool enumerateCameras(std::vector<hardware::CameraInfo> &cameras);
  bool setTorch(const std::string &camera_id, bool enabled);
  bool captureJpeg(const std::string &camera_id, const std::string &path,
                   hardware::PhotoResult &result, int max_width,
                   int max_height, bool flash, int timeout_ms);

  bool queryBattery(hardware::BatterySnapshot &snapshot);
  int waitForBatteryEvent(int timeout_ms, hardware::BatterySnapshot &snapshot);
  bool setInteractive(bool interactive);
  bool acquireWakeLock(const std::string &name);
  bool releaseWakeLock(const std::string &name);
  bool enableAutoSuspend();
  bool disableAutoSuspend();
  bool scheduleRtcWake(int delay_seconds);
  bool clearRtcWake();
  bool suspend(int graceful_timeout_ms);
  hardware::FlipState queryFlipState();

  bool vibrate(uint32_t duration_ms);
  bool stopVibration();
  bool supportsAmplitudeControl();
  bool setVibrationAmplitude(uint8_t amplitude);

  bool wifiStatus(network::WifiStatus &status);
  bool wifiScan(std::vector<network::WifiAccessPoint> &results, int wait_ms);
  bool wifiListNetworks(std::vector<network::WifiNetwork> &networks);
  bool wifiConnect(const std::string &ssid, network::WifiSecurity security,
                   const std::string &credential, int &network_id);
  bool wifiDisconnect();
  bool wifiReconnect();
  bool wifiForget(int network_id);
  bool wifiSaveConfiguration();

  bool ipStatus(network::IpConfiguration &configuration);
  bool ipUseDhcp(int timeout_ms);
  bool ipUseStatic(const network::IpConfiguration &configuration);

  bool bluetoothEnable(int timeout_ms);
  bool bluetoothDisable(int timeout_ms);
  bool bluetoothClassicScan(std::vector<network::BluetoothDevice> &devices,
                            int duration_ms);
  bool bluetoothLeScan(std::vector<network::BluetoothDevice> &devices,
                       int duration_ms);
  bool bluetoothPair(const std::string &address,
                     network::BluetoothTransport transport);
  bool bluetoothUnpair(const std::string &address);
  bool bluetoothCancelPairing(const std::string &address);
  bool bluetoothProfileConnect(const std::string &address,
                               network::BluetoothProfile profile);
  bool bluetoothProfileDisconnect(const std::string &address,
                                  network::BluetoothProfile profile);
  bool bluetoothProfileConnectionCycle(const std::string &address,
                                       network::BluetoothProfile profile,
                                       int hold_ms);
  bool bluetoothLeConnectionCycle(const std::string &address, int hold_ms,
                                  int timeout_ms);

  bool modemSnapshot(modem::ModemSnapshot &snapshot, int timeout_ms);
  bool setRadioPower(bool enabled, modem::ModemRequestStatus &status,
                     int timeout_ms);

  bool testH264RoundTrip(int width, int height, int frame_count,
                        hardware::CodecResult &result, int timeout_ms);

  const std::string &lastError() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::device
