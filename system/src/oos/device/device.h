#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace oos::device {

class Display;

} // namespace oos::device

namespace oos::input {
class KeyInputSource;
}

namespace oos::device {

enum class Feature : uint8_t {
  PrimaryDisplay,
  SecondaryDisplay,
  KeyInput,
  AudioPlayback,
  AudioCapture,
  CameraCapture,
  Torch,
  Vibration,
  Battery,
  Suspend,
  RtcWake,
  Wifi,
  IpConfiguration,
  BluetoothClassic,
  BluetoothLowEnergy,
  Modem,
  HardwareVideoCodec,
  Location,
  Sensors,
  FmRadio,
  Nfc,
  Count,
};

// Planned reserves an API slot without claiming that a backend works.
enum class CapabilityState : uint8_t {
  Unsupported,
  Planned,
  Implemented,
  Validated,
};

enum class WifiLifecycle : uint8_t {
  AndroidFramework,
  LegacyHardware,
};

struct DeviceDescriptor {
  const char *id = "";
  const char *manufacturer = "";
  const char *model = "";
  uint32_t android_api = 0;
  uint32_t primary_width = 0;
  uint32_t primary_height = 0;
  uint32_t secondary_width = 0;
  uint32_t secondary_height = 0;
};

// Values that vary while the public Manager APIs remain identical.
struct ServiceConfiguration {
  const char *input_directory = "/dev/input";
  const char *wifi_control_socket = "";
  const char *bluetooth_daemon = "";
  const char *modem_service = "slot1";
  const char *power_service = "default";
  const char *vibrator_service = "default";
  const char *primary_camera_id = "0";
  // Test backends may expose deterministic WIT service data without starting
  // platform HAL managers. Production device configurations leave this false.
  bool mock_hardware = false;
  WifiLifecycle wifi_lifecycle = WifiLifecycle::AndroidFramework;
};

struct DeviceInitOptions {
  bool primary_display = true;
  bool key_input = true;
  bool grab_input = true;
};

class Device {
public:
  virtual ~Device() = default;

  virtual const DeviceDescriptor &descriptor() const = 0;
  virtual const ServiceConfiguration &services() const = 0;
  virtual CapabilityState capability(Feature feature) const = 0;

  // Initializes only boot-critical resources selected by options. Other HAL
  // managers stay lazy and use services() when their owning subsystem starts.
  virtual bool initialize(const DeviceInitOptions &options = {}) = 0;
  virtual void shutdown() = 0;

  // Accessors require the corresponding initialize() option to have
  // completed successfully.
  virtual Display &display() = 0;
  virtual input::KeyInputSource &keyInput() = 0;
  virtual const std::string &lastError() const = 0;
};

// Exactly one target backend supplies this factory in each device build.
std::unique_ptr<Device> createDevice();

const char *featureName(Feature feature);
const char *capabilityStateName(CapabilityState state);
bool isImplemented(CapabilityState state);

} // namespace oos::device
