#include "oos/device/services.h"
#include "oos/hardware/audio_manager.h"
#include "oos/hardware/codec_manager.h"

#include <cstdio>
#include <memory>

int main() {
  std::unique_ptr<oos::device::Device> device = oos::device::createDevice();
  if (!device)
    return 1;

  oos::hardware::AudioManager audio;
  oos::hardware::CameraManager camera;
  oos::hardware::PowerManager power;
  oos::hardware::VibratorManager vibrator;
  auto wifi = oos::device::createWifiManager(*device);
  auto ip = oos::device::createIpManager(*device);
  oos::network::BluetoothManager bluetooth;
  oos::modem::ModemManager modem;
  (void)audio;
  (void)camera;
  (void)power;
  (void)vibrator;
  (void)wifi;
  (void)ip;
  (void)bluetooth;
  (void)modem;

  std::printf(
      "service-contract=%s hardware=%s network=%s modem=%s\n",
      device->descriptor().id,
      oos::device::isImplemented(
          device->capability(oos::device::Feature::AudioPlayback))
          ? "available"
          : "unavailable",
      oos::device::isImplemented(device->capability(oos::device::Feature::Wifi))
          ? "available"
          : "unavailable",
      oos::device::isImplemented(
          device->capability(oos::device::Feature::Modem))
          ? "available"
          : "unavailable");
  return 0;
}
