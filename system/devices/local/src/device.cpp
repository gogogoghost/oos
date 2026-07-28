#include "oos/device/device.h"

#include "oos/local/local_display.h"
#include "oos/local/local_key_input.h"

#include <cassert>
#include <cstdlib>
#include <memory>

namespace oos::device {
namespace {

constexpr DeviceDescriptor kDescriptor = {
    "local", "OOS", "Local Test Device", 0, 240, 320, 0, 0};

constexpr ServiceConfiguration kServices = {
    "local",      "mock:wlan0",    "mock:bluetooth", "mock:modem",
    "mock:power", "mock:vibrator", "mock:camera",    true,
};

const char *keymapPath() {
  const char *configured = std::getenv("OOS_LOCAL_KEYMAP");
  return configured && configured[0] ? configured : OOS_LOCAL_DEFAULT_KEYMAP;
}

class LocalDevice final : public Device {
public:
  ~LocalDevice() override { shutdown(); }

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
    case Feature::Suspend:
    case Feature::RtcWake:
    case Feature::Wifi:
    case Feature::IpConfiguration:
    case Feature::BluetoothClassic:
    case Feature::BluetoothLowEnergy:
    case Feature::Modem:
    case Feature::HardwareVideoCodec:
      return CapabilityState::Validated;
    case Feature::SecondaryDisplay:
    case Feature::Location:
    case Feature::Sensors:
    case Feature::FmRadio:
    case Feature::Nfc:
    case Feature::Count:
      return CapabilityState::Unsupported;
    }
    return CapabilityState::Unsupported;
  }

  bool initialize(const DeviceInitOptions &options) override {
    if (options.primary_display && !display_ready_) {
      if (!display_.initialize()) {
        error_ = "initialize local SDL/OpenGL display failed";
        return false;
      }
      display_ready_ = true;
    }
    if (options.key_input && !input_ready_) {
      if (!input_.initialize(keymapPath())) {
        error_ = input_.lastError();
        if (display_ready_) {
          display_.shutdown();
          display_ready_ = false;
        }
        return false;
      }
      input_ready_ = true;
    }
    error_.clear();
    return true;
  }

  void shutdown() override {
    if (input_ready_) {
      input_.shutdown();
      input_ready_ = false;
    }
    if (display_ready_) {
      display_.shutdown();
      display_ready_ = false;
    }
  }

  Display &display() override {
    assert(display_ready_);
    return display_;
  }

  input::KeyInputSource &keyInput() override {
    assert(input_ready_);
    return input_;
  }

  const std::string &lastError() const override { return error_; }

private:
  local::LocalDisplay display_;
  local::LocalKeyInput input_;
  bool display_ready_ = false;
  bool input_ready_ = false;
  std::string error_;
};

} // namespace

std::unique_ptr<Device> createDevice() {
  return std::make_unique<LocalDevice>();
}

} // namespace oos::device
