#include "oos/device/device.h"

namespace oos::device {

const char *featureName(Feature feature) {
  switch (feature) {
  case Feature::PrimaryDisplay:
    return "primary-display";
  case Feature::SecondaryDisplay:
    return "secondary-display";
  case Feature::KeyInput:
    return "key-input";
  case Feature::AudioPlayback:
    return "audio-playback";
  case Feature::AudioCapture:
    return "audio-capture";
  case Feature::CameraCapture:
    return "camera-capture";
  case Feature::Torch:
    return "torch";
  case Feature::Vibration:
    return "vibration";
  case Feature::Battery:
    return "battery";
  case Feature::Suspend:
    return "suspend";
  case Feature::RtcWake:
    return "rtc-wake";
  case Feature::Wifi:
    return "wifi";
  case Feature::IpConfiguration:
    return "ip-configuration";
  case Feature::BluetoothClassic:
    return "bluetooth-classic";
  case Feature::BluetoothLowEnergy:
    return "bluetooth-le";
  case Feature::Modem:
    return "modem";
  case Feature::HardwareVideoCodec:
    return "hardware-video-codec";
  case Feature::Location:
    return "location";
  case Feature::Sensors:
    return "sensors";
  case Feature::FmRadio:
    return "fm-radio";
  case Feature::Nfc:
    return "nfc";
  case Feature::Count:
    return "invalid";
  }
  return "invalid";
}

const char *capabilityStateName(CapabilityState state) {
  switch (state) {
  case CapabilityState::Unsupported:
    return "unsupported";
  case CapabilityState::Planned:
    return "planned";
  case CapabilityState::Implemented:
    return "implemented";
  case CapabilityState::Validated:
    return "validated";
  }
  return "invalid";
}

bool isImplemented(CapabilityState state) {
  return state == CapabilityState::Implemented ||
         state == CapabilityState::Validated;
}

} // namespace oos::device
