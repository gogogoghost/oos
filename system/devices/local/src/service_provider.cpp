#include "oos/device/service_provider.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <utility>

namespace oos::device {

struct ServiceProvider::Impl {
  explicit Impl(const Device &device)
      : device(device), error("mock service ready") {}

  const Device &device;
  std::string error;
  bool wifi_enabled = true;
  bool wifi_connected = true;
  int wifi_network_id = 1;
  int next_wifi_network_id = 2;
  std::string wifi_ssid = "OOS Mock Network";
  std::vector<network::WifiNetwork> wifi_networks = {
      {1, "OOS Mock Network", "02:00:00:00:00:01", "[CURRENT]"}};
  struct Pcm {
    hardware::PcmOutputConfig config;
    int64_t written = 0;
    float volume = 1.0F;
    bool app_paused = false;
    bool focus_paused = false;
    bool open = false;
  };
  std::array<Pcm, 8> pcm_outputs;
  mutable std::mutex pcm_mutex;
  bool audio_focused = true;
};

ServiceProvider::ServiceProvider(const Device &device)
    : impl_(std::make_unique<Impl>(device)) {}

ServiceProvider::~ServiceProvider() = default;

bool ServiceProvider::playTone(double, int duration_ms, float,
                               hardware::AudioUsage,
                               hardware::AudioStreamInfo &info) {
  info = {48000, 2, 1, static_cast<int64_t>(duration_ms) * 48};
  return true;
}

bool ServiceProvider::pcmOpen(const hardware::PcmOutputConfig &config,
                              uint32_t &handle,
                              hardware::AudioStreamInfo &info) {
  std::lock_guard<std::mutex> lock(impl_->pcm_mutex);
  handle = 0;
  if (config.sample_rate < 8000 || config.channel_count < 1 ||
      config.channel_count > 2 || config.capacity_frames < 256)
    return false;
  for (size_t index = 0; index < impl_->pcm_outputs.size(); ++index) {
    auto &stream = impl_->pcm_outputs[index];
    if (stream.open)
      continue;
    stream = {config, 0, 1.0F, false, !impl_->audio_focused, true};
    handle = static_cast<uint32_t>(index + 1);
    info = {config.sample_rate, config.channel_count, 1, 0};
    return true;
  }
  return false;
}

bool ServiceProvider::pcmWrite(uint32_t handle, const int16_t *samples,
                               int64_t frames, int64_t &accepted_frames) {
  std::lock_guard<std::mutex> lock(impl_->pcm_mutex);
  accepted_frames = 0;
  if (handle == 0 || handle > impl_->pcm_outputs.size() || !samples ||
      frames <= 0 || !impl_->pcm_outputs[handle - 1].open)
    return false;
  auto &stream = impl_->pcm_outputs[handle - 1];
  accepted_frames = std::min<int64_t>(frames, stream.config.capacity_frames);
  stream.written += accepted_frames;
  return true;
}

bool ServiceProvider::pcmSetVolume(uint32_t handle, float volume) {
  std::lock_guard<std::mutex> lock(impl_->pcm_mutex);
  if (handle == 0 || handle > impl_->pcm_outputs.size() || volume < 0.0F ||
      volume > 1.0F || !impl_->pcm_outputs[handle - 1].open)
    return false;
  impl_->pcm_outputs[handle - 1].volume = volume;
  return true;
}

bool ServiceProvider::pcmPause(uint32_t handle) {
  std::lock_guard<std::mutex> lock(impl_->pcm_mutex);
  if (handle == 0 || handle > impl_->pcm_outputs.size() ||
      !impl_->pcm_outputs[handle - 1].open)
    return false;
  impl_->pcm_outputs[handle - 1].app_paused = true;
  return true;
}

bool ServiceProvider::pcmResume(uint32_t handle) {
  std::lock_guard<std::mutex> lock(impl_->pcm_mutex);
  if (handle == 0 || handle > impl_->pcm_outputs.size() ||
      !impl_->pcm_outputs[handle - 1].open)
    return false;
  impl_->pcm_outputs[handle - 1].app_paused = false;
  return true;
}

bool ServiceProvider::pcmFlush(uint32_t handle) {
  std::lock_guard<std::mutex> lock(impl_->pcm_mutex);
  return handle != 0 && handle <= impl_->pcm_outputs.size() &&
         impl_->pcm_outputs[handle - 1].open;
}

bool ServiceProvider::pcmStatus(uint32_t handle,
                                hardware::PcmOutputStatus &status) {
  std::lock_guard<std::mutex> lock(impl_->pcm_mutex);
  if (handle == 0 || handle > impl_->pcm_outputs.size() ||
      !impl_->pcm_outputs[handle - 1].open)
    return false;
  const auto &stream = impl_->pcm_outputs[handle - 1];
  status = {{stream.config.sample_rate, stream.config.channel_count, 1,
             stream.written},
            0,
            stream.written,
            0,
            stream.app_paused || stream.focus_paused};
  return true;
}

bool ServiceProvider::pcmClose(uint32_t handle) {
  std::lock_guard<std::mutex> lock(impl_->pcm_mutex);
  if (handle == 0 || handle > impl_->pcm_outputs.size() ||
      !impl_->pcm_outputs[handle - 1].open)
    return false;
  impl_->pcm_outputs[handle - 1] = {};
  return true;
}

void ServiceProvider::setAudioFocused(bool focused) {
  std::lock_guard<std::mutex> lock(impl_->pcm_mutex);
  impl_->audio_focused = focused;
  for (auto &stream : impl_->pcm_outputs)
    if (stream.open)
      stream.focus_paused = !focused;
}

void ServiceProvider::closeAllPcm() {
  std::lock_guard<std::mutex> lock(impl_->pcm_mutex);
  for (auto &stream : impl_->pcm_outputs)
    stream = {};
}

bool ServiceProvider::recordWav(const std::string &path, int duration_ms,
                                hardware::RecordingResult &result) {
  (void)path;
  result = {{16000, 1, 2, static_cast<int64_t>(duration_ms) * 16},
            0.5,
            0.25,
            "/tmp/oos-local-recording.wav"};
  return true;
}

bool ServiceProvider::enumerateCameras(
    std::vector<hardware::CameraInfo> &cameras) {
  cameras = {
      {"mock-camera-0", hardware::LensFacing::Back, 90, 3, true, 1920, 1080}};
  return true;
}

bool ServiceProvider::setTorch(const std::string &, bool) { return true; }

bool ServiceProvider::captureJpeg(const std::string &, const std::string &path,
                                  hardware::PhotoResult &result, int max_width,
                                  int max_height, bool, int) {
  (void)path;
  result = {"/tmp/oos-local-photo.jpg", max_width, max_height, 4096};
  return true;
}

bool ServiceProvider::queryBattery(hardware::BatterySnapshot &snapshot) {
  snapshot = {
      hardware::BatteryState::Charging, 82, 4'050'000, 350'000, 250, true};
  return true;
}

int ServiceProvider::waitForBatteryEvent(int,
                                         hardware::BatterySnapshot &snapshot) {
  queryBattery(snapshot);
  return 0;
}

bool ServiceProvider::setInteractive(bool) { return true; }
bool ServiceProvider::acquireWakeLock(const std::string &) { return true; }
bool ServiceProvider::releaseWakeLock(const std::string &) { return true; }
bool ServiceProvider::enableAutoSuspend() { return true; }
bool ServiceProvider::disableAutoSuspend() { return true; }
bool ServiceProvider::scheduleRtcWake(int) { return true; }
bool ServiceProvider::clearRtcWake() { return true; }
bool ServiceProvider::suspend(int) { return true; }
hardware::FlipState ServiceProvider::queryFlipState() {
  return hardware::FlipState::Open;
}

bool ServiceProvider::vibrate(uint32_t) { return true; }
bool ServiceProvider::stopVibration() { return true; }
bool ServiceProvider::supportsAmplitudeControl() { return true; }
bool ServiceProvider::setVibrationAmplitude(uint8_t) { return true; }

bool ServiceProvider::wifiStatus(network::WifiStatus &status) {
  if (!impl_->wifi_enabled) {
    impl_->error = "Wi-Fi is disabled";
    return false;
  }
  status = impl_->wifi_connected
               ? network::WifiStatus{"COMPLETED", impl_->wifi_ssid,
                                     "02:00:00:00:00:01", "192.0.2.2",
                                     impl_->wifi_network_id}
               : network::WifiStatus{"DISCONNECTED", "", "", "", -1};
  return true;
}

bool ServiceProvider::wifiEnabled(bool &enabled) {
  enabled = impl_->wifi_enabled;
  return true;
}

bool ServiceProvider::wifiSetEnabled(bool enabled) {
  impl_->wifi_enabled = enabled;
  if (!enabled)
    impl_->wifi_connected = false;
  return true;
}

bool ServiceProvider::wifiScan(std::vector<network::WifiAccessPoint> &results,
                               int) {
  if (!impl_->wifi_enabled) {
    impl_->error = "Wi-Fi is disabled";
    return false;
  }
  results = {
      {"02:00:00:00:00:01", 2412, -42, "[WPA2-PSK-CCMP][ESS]",
       "OOS Mock Network"},
      {"02:00:00:00:00:02", 2437, -58, "[WPA2-PSK-CCMP][ESS]", "Orange Lab"},
      {"02:00:00:00:00:03", 2462, -71, "[ESS]", "橙子实验室访客网络"}};
  return true;
}

bool ServiceProvider::wifiListNetworks(
    std::vector<network::WifiNetwork> &networks) {
  networks = impl_->wifi_networks;
  return true;
}

bool ServiceProvider::wifiConnect(const std::string &ssid,
                                  network::WifiSecurity, const std::string &,
                                  int &network_id) {
  if (!impl_->wifi_enabled)
    return false;
  network_id = impl_->next_wifi_network_id++;
  impl_->wifi_network_id = network_id;
  impl_->wifi_ssid = ssid;
  impl_->wifi_connected = true;
  impl_->wifi_networks.push_back({network_id, ssid, "any", "[CURRENT]"});
  return true;
}

bool ServiceProvider::wifiSelect(int network_id) {
  if (!impl_->wifi_enabled || network_id < 0)
    return false;
  const auto network =
      std::find_if(impl_->wifi_networks.begin(), impl_->wifi_networks.end(),
                   [&](const network::WifiNetwork &candidate) {
                     return candidate.id == network_id;
                   });
  if (network == impl_->wifi_networks.end())
    return false;
  impl_->wifi_network_id = network_id;
  impl_->wifi_ssid = network->ssid;
  impl_->wifi_connected = true;
  return true;
}

bool ServiceProvider::wifiDisconnect() {
  impl_->wifi_connected = false;
  return true;
}
bool ServiceProvider::wifiReconnect() {
  impl_->wifi_connected = impl_->wifi_enabled;
  return impl_->wifi_enabled;
}
bool ServiceProvider::wifiForget(int network_id) {
  if (impl_->wifi_network_id == network_id) {
    impl_->wifi_network_id = -1;
    impl_->wifi_connected = false;
  }
  impl_->wifi_networks.erase(
      std::remove_if(impl_->wifi_networks.begin(), impl_->wifi_networks.end(),
                     [&](const network::WifiNetwork &network) {
                       return network.id == network_id;
                     }),
      impl_->wifi_networks.end());
  return true;
}
bool ServiceProvider::wifiSaveConfiguration() { return true; }

bool ServiceProvider::ipStatus(network::IpConfiguration &configuration) {
  configuration = {"wlan0",     "192.0.2.2",  24,
                   "192.0.2.1", "192.0.2.53", "198.51.100.53"};
  return true;
}

bool ServiceProvider::ipUseDhcp(int) { return true; }
bool ServiceProvider::ipUseStatic(const network::IpConfiguration &) {
  return true;
}

bool ServiceProvider::bluetoothEnable(int) { return true; }
bool ServiceProvider::bluetoothDisable(int) { return true; }

bool ServiceProvider::bluetoothClassicScan(
    std::vector<network::BluetoothDevice> &devices, int) {
  devices = {{"02:00:00:00:00:02", "OOS Mock Headset", -38, 0x240404, 3, {0}}};
  return true;
}

bool ServiceProvider::bluetoothLeScan(
    std::vector<network::BluetoothDevice> &devices, int duration_ms) {
  return bluetoothClassicScan(devices, duration_ms);
}

bool ServiceProvider::bluetoothPair(const std::string &,
                                    network::BluetoothTransport) {
  return true;
}
bool ServiceProvider::bluetoothUnpair(const std::string &) { return true; }
bool ServiceProvider::bluetoothCancelPairing(const std::string &) {
  return true;
}
bool ServiceProvider::bluetoothProfileConnect(const std::string &,
                                              network::BluetoothProfile) {
  return true;
}
bool ServiceProvider::bluetoothProfileDisconnect(const std::string &,
                                                 network::BluetoothProfile) {
  return true;
}
bool ServiceProvider::bluetoothProfileConnectionCycle(const std::string &,
                                                      network::BluetoothProfile,
                                                      int) {
  return true;
}
bool ServiceProvider::bluetoothLeConnectionCycle(const std::string &, int,
                                                 int) {
  return true;
}

bool ServiceProvider::modemSnapshot(modem::ModemSnapshot &snapshot, int) {
  snapshot = {};
  snapshot.service_connected = true;
  snapshot.radio_state = 1;
  snapshot.baseband_version = "OOS-MOCK-1.0";
  snapshot.identity = {"000000000000000", "00", "00000000", "00000000000000"};
  snapshot.sim = {1, 0, 1};
  snapshot.signal.gsm_strength = 20;
  snapshot.signal.gsm_bit_error_rate = 0;
  snapshot.signal.lte_strength = 30;
  snapshot.signal.lte_rsrp = -95;
  snapshot.signal.lte_rsrq = -10;
  snapshot.signal.lte_rssnr = 45;
  snapshot.signal.lte_cqi = 12;
  snapshot.voice_registration = {1, 14, 0, 1};
  snapshot.data_registration = {1, 14, 0, 1};
  snapshot.network_operator = {"OOS Mock Carrier", "OOS", "00101"};
  snapshot.preferred_network_type = 9;
  snapshot.voice_radio_technology = 14;
  snapshot.current_call_count = 0;
  snapshot.data_call_count = 0;
  snapshot.hardware_config_count = 1;
  snapshot.radio_access_family = 0x1000;
  snapshot.logical_modem_uuid = "00000000-0000-0000-0000-000000000001";
  return true;
}

bool ServiceProvider::modemNetworkStatus(modem::NetworkStatus &status, int) {
  status = {};
  status.service_connected = true;
  status.radio_state = 1;
  status.signal.gsm_strength = 20;
  status.signal.lte_strength = 30;
  status.signal.lte_rsrp = -95;
  status.voice_registration = {1, 14, 0, 1};
  status.data_registration = {1, 14, 0, 1};
  return true;
}

bool ServiceProvider::setRadioPower(bool, modem::ModemRequestStatus &status,
                                    int) {
  status = {"set-radio-power", 0, false};
  return true;
}

bool ServiceProvider::testH264RoundTrip(int width, int height, int frame_count,
                                        hardware::CodecResult &result, int) {
  result = {"mock.h264.encoder",
            "mock.h264.decoder",
            true,
            true,
            width,
            height,
            frame_count,
            frame_count,
            frame_count,
            static_cast<size_t>(frame_count) * 1024};
  return true;
}

std::string ServiceProvider::lastError() const {
  std::lock_guard<std::mutex> lock(impl_->pcm_mutex);
  return impl_->error;
}

} // namespace oos::device
