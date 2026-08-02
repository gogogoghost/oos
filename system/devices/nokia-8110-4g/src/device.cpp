#include "oos/device/device.h"

#include "oos/input/key_input.h"
#include "oos/nokia8110/primary_gles_display.h"

#include <cassert>
#include <memory>
#include <utility>

namespace oos::device {
namespace {

constexpr DeviceDescriptor kDescriptor = {
    "nokia-8110-4g", "Nokia", "8110 4G", 23, 240, 320, 0, 0};

constexpr ServiceConfiguration kServices = {
    "/dev/input",
    "/data/misc/wifi/sockets/wlan0",
    "bluetoothd",
    "slot1",
    "default",
    "default",
    "0",
    false,
    WifiLifecycle::LegacyHardware,
};

class Nokia8110Device final : public Device {
public:
  ~Nokia8110Device() override { shutdown(); }

  const DeviceDescriptor &descriptor() const override { return kDescriptor; }
  const ServiceConfiguration &services() const override { return kServices; }

  CapabilityState capability(Feature feature) const override {
    switch (feature) {
    case Feature::PrimaryDisplay:
    case Feature::KeyInput:
    case Feature::AudioPlayback:
    case Feature::AudioCapture:
    case Feature::CameraCapture:
    case Feature::Torch:
    case Feature::Vibration:
    case Feature::Battery:
    case Feature::Wifi:
    case Feature::IpConfiguration:
    case Feature::BluetoothClassic:
    case Feature::BluetoothLowEnergy:
    case Feature::Modem:
      return CapabilityState::Validated;
    case Feature::Suspend:
    case Feature::RtcWake:
      return CapabilityState::Implemented;
    case Feature::HardwareVideoCodec:
    case Feature::Location:
    case Feature::Sensors:
    case Feature::FmRadio:
      return CapabilityState::Planned;
    case Feature::SecondaryDisplay:
    case Feature::Nfc:
    case Feature::Count:
      return CapabilityState::Unsupported;
    }
    return CapabilityState::Unsupported;
  }

  bool initialize(const DeviceInitOptions &options) override {
    bool display_started = false;
    if (options.primary_display && !display_ready_) {
      if (!display_.initialize()) {
        error_ = "initialize primary display failed";
        return false;
      }
      display_ready_ = true;
      display_started = true;
    }
    if (options.key_input && !input_) {
      auto key_input = std::make_unique<input::KeyInput>(input::KeyInputOptions{
          options.grab_input,
      });
      if (!key_input->initialize(kServices.input_directory)) {
        error_ = "initialize key input failed";
        if (display_started) {
          display_.shutdown();
          display_ready_ = false;
        }
        return false;
      }
      input_ = std::move(key_input);
    }
    error_.clear();
    return true;
  }

  void shutdown() override {
    if (input_) {
      input_->shutdown();
      input_.reset();
    }
    if (display_ready_) {
      display_.shutdown();
      display_ready_ = false;
    }
  }

  Display &display() override { return display_; }

  input::KeyInputSource &keyInput() override {
    assert(input_);
    return *input_;
  }

  const std::string &lastError() const override { return error_; }

private:
  nokia8110::PrimaryGlesDisplay display_;
  std::unique_ptr<input::KeyInput> input_;
  bool display_ready_ = false;
  std::string error_;
};

} // namespace

std::unique_ptr<Device> createDevice() {
  return std::make_unique<Nokia8110Device>();
}

} // namespace oos::device
