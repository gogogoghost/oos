#include "oos/device/device.h"
#include "oos/device/display.h"

#include <cstdio>
#include <memory>

int main() {
  std::unique_ptr<oos::device::Device> device = oos::device::createDevice();
  if (!device) {
    std::fprintf(stderr, "device factory returned null\n");
    return 1;
  }

  const auto &descriptor = device->descriptor();
  const auto &services = device->services();
  if (!descriptor.id || !descriptor.id[0] || !descriptor.model ||
      !descriptor.model[0] || descriptor.primary_width == 0 ||
      descriptor.primary_height == 0 ||
      descriptor.primary_width != device->display().width() ||
      descriptor.primary_height != device->display().height() ||
      !services.input_directory || !services.input_directory[0]) {
    std::fprintf(stderr,
                 "invalid device descriptor or service configuration\n");
    return 1;
  }
  if (!oos::device::isImplemented(
          device->capability(oos::device::Feature::PrimaryDisplay)) ||
      !oos::device::isImplemented(
          device->capability(oos::device::Feature::KeyInput))) {
    std::fprintf(stderr, "boot-critical capabilities are not implemented\n");
    return 1;
  }

  std::printf("device=%s manufacturer=%s model=%s android_api=%u "
              "primary=%ux%u secondary=%ux%u\n",
              descriptor.id, descriptor.manufacturer, descriptor.model,
              descriptor.android_api, descriptor.primary_width,
              descriptor.primary_height, descriptor.secondary_width,
              descriptor.secondary_height);
  std::printf("service.input=%s service.wifi=%s service.bluetooth=%s "
              "service.modem=%s\n",
              services.input_directory, services.wifi_control_socket,
              services.bluetooth_daemon, services.modem_service);
  for (uint8_t raw = 0; raw < static_cast<uint8_t>(oos::device::Feature::Count);
       ++raw) {
    const auto feature = static_cast<oos::device::Feature>(raw);
    std::printf("feature.%s=%s\n", oos::device::featureName(feature),
                oos::device::capabilityStateName(device->capability(feature)));
  }
  return 0;
}
