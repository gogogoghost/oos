#include "oos/web/device_api_service.h"

#include "oos/apps/json.h"
#include "oos/apps/permissions.h"
#include "oos/device/device.h"
#include "oos/device/service_provider.h"
#include "oos/services/system_service.h"
#include "oos/storage/app_storage.h"
#include "oos/storage/device_storage.h"
#include "oos/web/device_api_transport.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string_view>
#include <vector>

namespace oos::web {
namespace {

constexpr int kReplyTimeoutMs = 30000;

std::string errorText(const char *operation, int result) {
  return std::string(operation) + ": " + std::strerror(-result);
}

void appendJsonString(std::string &output, const std::string &value) {
  static const char hex[] = "0123456789abcdef";
  output.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20) {
        output += "\\u00";
        output.push_back(hex[character >> 4]);
        output.push_back(hex[character & 0x0f]);
      } else {
        output.push_back(static_cast<char>(character));
      }
    }
  }
  output.push_back('"');
}

std::string
serializeEntries(const std::vector<storage::DeviceStorageEntry> &entries) {
  std::string output = "[";
  for (size_t index = 0; index < entries.size(); ++index) {
    if (index)
      output.push_back(',');
    output += "{\"path\":";
    appendJsonString(output, entries[index].path);
    output += ",\"size\":" + std::to_string(entries[index].size);
    output +=
        ",\"lastModified\":" + std::to_string(entries[index].last_modified_ms) +
        "}";
  }
  output.push_back(']');
  return output;
}

bool hasPermission(const DeviceApiContext *context,
                   apps::DeviceServicePermission permission) {
  return context &&
         apps::hasDeviceServicePermission(context->permission_mask, permission);
}

bool fieldString(const apps::JsonValue &root, const char *name,
                 std::string &value, size_t maximum = 4096) {
  const apps::JsonValue *field = root.get(name);
  if (!field || !field->isString() || field->stringValue().size() > maximum)
    return false;
  value = field->stringValue();
  return true;
}

bool fieldInteger(const apps::JsonValue &root, const char *name, int64_t &value,
                  int64_t minimum, int64_t maximum) {
  const apps::JsonValue *field = root.get(name);
  if (!field || !field->isNumber() || field->integerValue() < minimum ||
      field->integerValue() > maximum)
    return false;
  value = field->integerValue();
  return true;
}

bool fieldBoolean(const apps::JsonValue &root, const char *name, bool &value) {
  const apps::JsonValue *field = root.get(name);
  if (!field || !field->isBoolean())
    return false;
  value = field->booleanValue();
  return true;
}

void appendKey(std::string &output, const char *key) {
  if (output.back() != '{' && output.back() != '[')
    output.push_back(',');
  appendJsonString(output, key);
  output.push_back(':');
}

void appendStringField(std::string &output, const char *key,
                       const std::string &value) {
  appendKey(output, key);
  appendJsonString(output, value);
}

void appendIntegerField(std::string &output, const char *key, int64_t value) {
  appendKey(output, key);
  output += std::to_string(value);
}

void appendBooleanField(std::string &output, const char *key, bool value) {
  appendKey(output, key);
  output += value ? "true" : "false";
}

const char *jsonBatteryStateName(hardware::BatteryState state) {
  switch (state) {
  case hardware::BatteryState::Charging:
    return "charging";
  case hardware::BatteryState::Discharging:
    return "discharging";
  case hardware::BatteryState::NotCharging:
    return "not-charging";
  case hardware::BatteryState::Full:
    return "full";
  case hardware::BatteryState::Unknown:
    return "unknown";
  }
  return "unknown";
}

const char *jsonFlipStateName(hardware::FlipState state) {
  switch (state) {
  case hardware::FlipState::Open:
    return "open";
  case hardware::FlipState::Closed:
    return "closed";
  case hardware::FlipState::Unknown:
    return "unknown";
  }
  return "unknown";
}

const char *jsonLensFacingName(hardware::LensFacing facing) {
  switch (facing) {
  case hardware::LensFacing::Front:
    return "front";
  case hardware::LensFacing::Back:
    return "back";
  case hardware::LensFacing::External:
    return "external";
  case hardware::LensFacing::Unknown:
    return "unknown";
  }
  return "unknown";
}

std::string serializeBattery(const hardware::BatterySnapshot &battery) {
  std::string output = "{";
  appendStringField(output, "state", jsonBatteryStateName(battery.state));
  appendIntegerField(output, "capacityPercent", battery.capacity_percent);
  appendIntegerField(output, "voltageMicrovolts", battery.voltage_microvolts);
  appendIntegerField(output, "currentMicroamps", battery.current_microamps);
  appendIntegerField(output, "temperatureTenthsCelsius",
                     battery.temperature_tenths_celsius);
  appendBooleanField(output, "usbOnline", battery.usb_online);
  output.push_back('}');
  return output;
}

std::string serializeWifiStatus(const network::WifiStatus &status) {
  std::string output = "{";
  appendStringField(output, "state", status.state);
  appendStringField(output, "ssid", status.ssid);
  appendStringField(output, "bssid", status.bssid);
  appendStringField(output, "ipAddress", status.ip_address);
  appendIntegerField(output, "networkId", status.network_id);
  output.push_back('}');
  return output;
}

std::string serializeWifiScan(
    const std::vector<network::WifiAccessPoint> &access_points) {
  std::string output = "[";
  for (const auto &access_point : access_points) {
    if (output.back() != '[')
      output.push_back(',');
    output.push_back('{');
    appendStringField(output, "ssid", access_point.ssid);
    appendStringField(output, "bssid", access_point.bssid);
    appendIntegerField(output, "frequencyMhz", access_point.frequency_mhz);
    appendIntegerField(output, "signalDbm", access_point.signal_dbm);
    appendStringField(output, "capabilities", access_point.flags);
    output.push_back('}');
  }
  output.push_back(']');
  return output;
}

std::string
serializeWifiNetworks(const std::vector<network::WifiNetwork> &networks) {
  std::string output = "[";
  for (const auto &network : networks) {
    if (output.back() != '[')
      output.push_back(',');
    output.push_back('{');
    appendIntegerField(output, "networkId", network.id);
    appendStringField(output, "ssid", network.ssid);
    appendStringField(output, "bssid", network.bssid);
    appendStringField(output, "capabilities", network.flags);
    output.push_back('}');
  }
  output.push_back(']');
  return output;
}

std::string serializeIp(const network::IpConfiguration &configuration) {
  std::string output = "{";
  appendStringField(output, "interfaceName", configuration.interface_name);
  appendStringField(output, "address", configuration.address);
  appendIntegerField(output, "prefixLength", configuration.prefix_length);
  appendStringField(output, "gateway", configuration.gateway);
  appendStringField(output, "dns1", configuration.dns1);
  appendStringField(output, "dns2", configuration.dns2);
  output.push_back('}');
  return output;
}

std::string serializeBluetooth(
    const std::vector<network::BluetoothDevice> &devices) {
  std::string output = "[";
  for (const auto &device : devices) {
    if (output.back() != '[')
      output.push_back(',');
    output.push_back('{');
    appendStringField(output, "address", device.address);
    appendStringField(output, "name", device.name);
    appendIntegerField(output, "rssi", device.rssi);
    appendIntegerField(output, "deviceClass", device.device_class);
    appendIntegerField(output, "deviceType", device.device_type);
    appendKey(output, "advertisingData");
    output.push_back('[');
    for (const uint8_t byte : device.advertising_data) {
      if (output.back() != '[')
        output.push_back(',');
      output += std::to_string(byte);
    }
    output += "]}";
  }
  output.push_back(']');
  return output;
}

std::string serializeCameras(const std::vector<hardware::CameraInfo> &cameras) {
  std::string output = "[";
  for (const auto &camera : cameras) {
    if (output.back() != '[')
      output.push_back(',');
    output.push_back('{');
    appendStringField(output, "id", camera.id);
    appendStringField(output, "facing", jsonLensFacingName(camera.facing));
    appendIntegerField(output, "sensorOrientation", camera.sensor_orientation);
    appendIntegerField(output, "hardwareLevel", camera.hardware_level);
    appendBooleanField(output, "flashAvailable", camera.flash_available);
    appendIntegerField(output, "maxJpegWidth", camera.max_jpeg_width);
    appendIntegerField(output, "maxJpegHeight", camera.max_jpeg_height);
    output.push_back('}');
  }
  output.push_back(']');
  return output;
}

std::string serializeModem(const modem::ModemSnapshot &snapshot) {
  std::string output = "{";
  appendBooleanField(output, "serviceConnected", snapshot.service_connected);
  appendIntegerField(output, "radioState", snapshot.radio_state);
  appendStringField(output, "basebandVersion", snapshot.baseband_version);
  appendKey(output, "identity");
  output.push_back('{');
  appendStringField(output, "imei", snapshot.identity.imei);
  appendStringField(output, "imeiSoftwareVersion",
                    snapshot.identity.imei_software_version);
  appendStringField(output, "esn", snapshot.identity.esn);
  appendStringField(output, "meid", snapshot.identity.meid);
  output.push_back('}');
  appendKey(output, "sim");
  output.push_back('{');
  appendIntegerField(output, "cardState", snapshot.sim.card_state);
  appendIntegerField(output, "universalPinState",
                     snapshot.sim.universal_pin_state);
  appendIntegerField(output, "applicationCount",
                     snapshot.sim.application_count);
  output.push_back('}');
  appendKey(output, "signal");
  output.push_back('{');
  appendIntegerField(output, "gsmStrength", snapshot.signal.gsm_strength);
  appendIntegerField(output, "gsmBitErrorRate",
                     snapshot.signal.gsm_bit_error_rate);
  appendIntegerField(output, "lteStrength", snapshot.signal.lte_strength);
  appendIntegerField(output, "lteRsrp", snapshot.signal.lte_rsrp);
  appendIntegerField(output, "lteRsrq", snapshot.signal.lte_rsrq);
  appendIntegerField(output, "lteRssnr", snapshot.signal.lte_rssnr);
  output.push_back('}');
  appendKey(output, "voiceRegistration");
  output.push_back('{');
  appendIntegerField(output, "state", snapshot.voice_registration.state);
  appendIntegerField(output, "radioTechnology",
                     snapshot.voice_registration.radio_technology);
  output.push_back('}');
  appendKey(output, "dataRegistration");
  output.push_back('{');
  appendIntegerField(output, "state", snapshot.data_registration.state);
  appendIntegerField(output, "radioTechnology",
                     snapshot.data_registration.radio_technology);
  output.push_back('}');
  appendKey(output, "networkOperator");
  output.push_back('{');
  appendStringField(output, "longName", snapshot.network_operator.long_name);
  appendStringField(output, "shortName", snapshot.network_operator.short_name);
  appendStringField(output, "numeric", snapshot.network_operator.numeric);
  output.push_back('}');
  appendIntegerField(output, "preferredNetworkType",
                     snapshot.preferred_network_type);
  appendIntegerField(output, "voiceRadioTechnology",
                     snapshot.voice_radio_technology);
  appendIntegerField(output, "currentCallCount", snapshot.current_call_count);
  appendIntegerField(output, "dataCallCount", snapshot.data_call_count);
  output.push_back('}');
  return output;
}

int servicePlatformCall(const OosDeviceApiRequest &request,
                        DeviceApiContext *context,
                        std::string &response) {
  if (!context || !context->services || !context->device)
    return -ENOSYS;
  if (request.payload_size > 256 * 1024)
    return -E2BIG;
  apps::JsonValue arguments;
  std::string parse_error;
  std::string encoded;
  if (request.payload_size)
    encoded.assign(static_cast<const char *>(request.payload),
                   request.payload_size);
  if (!apps::parseJson(encoded.empty() ? "{}" : encoded, arguments,
                       parse_error) ||
      !arguments.isObject())
    return -EINVAL;
  device::ServiceProvider &services = *context->services;
  const std::string_view method(request.path);

  if (method == "system.request") {
    if (!context->system_services || context->app_id.empty())
      return -ENOSYS;
    std::string service;
    std::string operation;
    std::string payload;
    if (!fieldString(arguments, "service", service, 64) || service.empty() ||
        !fieldString(arguments, "operation", operation, 64) ||
        operation.empty() || !fieldString(arguments, "payload", payload,
                                           256 * 1024))
      return -EINVAL;
    return context->system_services->request(
        context->app_id, context->permissions, service, operation, payload,
        response);
  }

  if (method == "datastore.get" || method == "datastore.set") {
    std::string name;
    if (!context->app_storage ||
        !fieldString(arguments, "name", name, 128) || name.empty())
      return -EINVAL;
    const auto grant = context->owned_data_stores.find(name);
    if (grant == context->owned_data_stores.end())
      return -EACCES;
    const std::string key = "kaios-datastore:" + name;
    if (method == "datastore.get") {
      std::vector<uint8_t> bytes;
      bool found = false;
      if (!context->app_storage->get(key, bytes, found))
        return -EIO;
      response = "{\"found\":";
      response += found ? "true" : "false";
      response += ",\"value\":";
      appendJsonString(response,
                       found ? std::string(bytes.begin(), bytes.end()) : "");
      response.push_back('}');
      return 0;
    }
    if (!grant->second)
      return -EACCES;
    std::string value;
    if (!fieldString(arguments, "value", value, 240 * 1024))
      return -EINVAL;
    if (!context->app_storage->set(
            key, reinterpret_cast<const uint8_t *>(value.data()), value.size()))
      return -EIO;
    response = "null";
    return 0;
  }

  if (method == "device.describe") {
    const device::DeviceDescriptor &descriptor = context->device->descriptor();
    response = "{";
    appendStringField(response, "id", descriptor.id);
    appendStringField(response, "manufacturer", descriptor.manufacturer);
    appendStringField(response, "model", descriptor.model);
    appendIntegerField(response, "androidApi", descriptor.android_api);
    appendIntegerField(response, "primaryWidth", descriptor.primary_width);
    appendIntegerField(response, "primaryHeight", descriptor.primary_height);
    appendIntegerField(response, "secondaryWidth", descriptor.secondary_width);
    appendIntegerField(response, "secondaryHeight", descriptor.secondary_height);
    response.push_back('}');
    return 0;
  }
  if (method == "device.capabilities") {
    response = "{";
    for (uint8_t index = 0;
         index < static_cast<uint8_t>(device::Feature::Count); ++index) {
      const auto feature = static_cast<device::Feature>(index);
      appendStringField(response, device::featureName(feature),
                        device::capabilityStateName(
                            context->device->capability(feature)));
    }
    response.push_back('}');
    return 0;
  }

  if (method == "power.battery") {
    hardware::BatterySnapshot battery;
    if (!services.queryBattery(battery))
      return -EIO;
    response = serializeBattery(battery);
    return 0;
  }
  if (method == "power.acquire-wake-lock" ||
      method == "power.release-wake-lock") {
    if (!hasPermission(context, apps::DeviceServicePermission::Power))
      return -EACCES;
    std::string name;
    if (!fieldString(arguments, "name", name, 128) || name.empty())
      return -EINVAL;
    auto held = context->wake_locks.end();
    if (method == "power.release-wake-lock") {
      held = std::find(context->wake_locks.begin(),
                       context->wake_locks.end(), name);
      if (held == context->wake_locks.end())
        return -EPERM;
    }
    const bool success = method == "power.acquire-wake-lock"
                             ? services.acquireWakeLock(name)
                             : services.releaseWakeLock(name);
    if (!success)
      return -EIO;
    if (method == "power.acquire-wake-lock") {
      context->wake_locks.push_back(name);
    } else {
      context->wake_locks.erase(held);
    }
    response = "null";
    return 0;
  }
  if (method == "power.flip-state") {
    if (!hasPermission(context, apps::DeviceServicePermission::Power))
      return -EACCES;
    response = "\"";
    response += jsonFlipStateName(services.queryFlipState());
    response.push_back('"');
    return 0;
  }

  if (method == "vibrator.vibrate" || method == "vibrator.stop") {
    int64_t duration_ms = 0;
    if (method == "vibrator.vibrate" &&
        !fieldInteger(arguments, "durationMs", duration_ms, 0, 60000))
      return -EINVAL;
    const bool success = method == "vibrator.vibrate"
                             ? services.vibrate(duration_ms)
                             : services.stopVibration();
    if (!success)
      return -EIO;
    response = "null";
    return 0;
  }

  if (method.compare(0, 5, "wifi.") == 0 ||
      method.compare(0, 3, "ip.") == 0) {
    if (context->restrict_connectivity)
      return -ENOTSUP;
    if (!hasPermission(context, apps::DeviceServicePermission::Wifi))
      return -EACCES;
    if (method == "wifi.status") {
      network::WifiStatus status;
      if (!services.wifiStatus(status))
        return -EIO;
      response = serializeWifiStatus(status);
      return 0;
    }
    if (method == "wifi.scan") {
      int64_t timeout_ms = 3000;
      const apps::JsonValue *timeout = arguments.get("timeoutMs");
      if (timeout &&
          (!timeout->isNumber() || timeout->integerValue() < 0 ||
           timeout->integerValue() > 30000))
        return -EINVAL;
      if (timeout)
        timeout_ms = timeout->integerValue();
      std::vector<network::WifiAccessPoint> access_points;
      if (!services.wifiScan(access_points, timeout_ms))
        return -EIO;
      response = serializeWifiScan(access_points);
      return 0;
    }
    if (method == "wifi.networks") {
      std::vector<network::WifiNetwork> networks;
      if (!services.wifiListNetworks(networks))
        return -EIO;
      response = serializeWifiNetworks(networks);
      return 0;
    }
    if (method == "wifi.connect") {
      std::string ssid;
      std::string credential;
      int64_t security = 0;
      if (!fieldString(arguments, "ssid", ssid, 256) || ssid.empty() ||
          !fieldString(arguments, "credential", credential, 4096) ||
          !fieldInteger(arguments, "security", security, 0, 1))
        return -EINVAL;
      int network_id = -1;
      if (!services.wifiConnect(
              ssid, static_cast<network::WifiSecurity>(security), credential,
              network_id))
        return -EIO;
      response = std::to_string(network_id);
      return 0;
    }
    if (method == "wifi.disconnect" || method == "wifi.reconnect" ||
        method == "wifi.save") {
      const bool success = method == "wifi.disconnect"
                               ? services.wifiDisconnect()
                           : method == "wifi.reconnect"
                               ? services.wifiReconnect()
                               : services.wifiSaveConfiguration();
      if (!success)
        return -EIO;
      response = "null";
      return 0;
    }
    if (method == "wifi.forget") {
      int64_t network_id = -1;
      if (!fieldInteger(arguments, "networkId", network_id, -1, INT32_MAX))
        return -EINVAL;
      if (!services.wifiForget(network_id))
        return -EIO;
      response = "null";
      return 0;
    }
    if (method == "ip.status") {
      network::IpConfiguration configuration;
      if (!services.ipStatus(configuration))
        return -EIO;
      response = serializeIp(configuration);
      return 0;
    }
    if (method == "ip.dhcp") {
      int64_t timeout_ms = 15000;
      const apps::JsonValue *timeout = arguments.get("timeoutMs");
      if (timeout &&
          (!timeout->isNumber() || timeout->integerValue() < 0 ||
           timeout->integerValue() > 60000))
        return -EINVAL;
      if (timeout)
        timeout_ms = timeout->integerValue();
      if (!services.ipUseDhcp(timeout_ms))
        return -EIO;
      response = "null";
      return 0;
    }
    if (method == "ip.static") {
      network::IpConfiguration configuration;
      int64_t prefix_length = 0;
      if (!fieldString(arguments, "interfaceName",
                       configuration.interface_name, 32) ||
          !fieldString(arguments, "address", configuration.address, 64) ||
          !fieldInteger(arguments, "prefixLength", prefix_length, 0, 128) ||
          !fieldString(arguments, "gateway", configuration.gateway, 64) ||
          !fieldString(arguments, "dns1", configuration.dns1, 64) ||
          !fieldString(arguments, "dns2", configuration.dns2, 64))
        return -EINVAL;
      configuration.prefix_length = static_cast<uint32_t>(prefix_length);
      if (!services.ipUseStatic(configuration))
        return -EIO;
      response = "null";
      return 0;
    }
  }

  if (method.compare(0, 10, "bluetooth.") == 0) {
    if (context->restrict_connectivity)
      return -ENOTSUP;
    if (!hasPermission(context, apps::DeviceServicePermission::Bluetooth))
      return -EACCES;
    if (method == "bluetooth.enable" || method == "bluetooth.disable") {
      int64_t timeout_ms = 10000;
      const apps::JsonValue *timeout = arguments.get("timeoutMs");
      if (timeout &&
          (!timeout->isNumber() || timeout->integerValue() < 0 ||
           timeout->integerValue() > 60000))
        return -EINVAL;
      if (timeout)
        timeout_ms = timeout->integerValue();
      const bool success = method == "bluetooth.enable"
                               ? services.bluetoothEnable(timeout_ms)
                               : services.bluetoothDisable(timeout_ms);
      if (!success)
        return -EIO;
      response = "null";
      return 0;
    }
    if (method == "bluetooth.classic-scan" ||
        method == "bluetooth.le-scan") {
      int64_t duration_ms = 5000;
      const apps::JsonValue *duration = arguments.get("durationMs");
      if (duration &&
          (!duration->isNumber() || duration->integerValue() < 0 ||
           duration->integerValue() > 60000))
        return -EINVAL;
      if (duration)
        duration_ms = duration->integerValue();
      std::vector<network::BluetoothDevice> devices;
      const bool success = method == "bluetooth.classic-scan"
                               ? services.bluetoothClassicScan(devices,
                                                               duration_ms)
                               : services.bluetoothLeScan(devices, duration_ms);
      if (!success)
        return -EIO;
      response = serializeBluetooth(devices);
      return 0;
    }
    std::string address;
    if (!fieldString(arguments, "address", address, 32) || address.empty())
      return -EINVAL;
    if (method == "bluetooth.pair") {
      int64_t transport = 0;
      if (!fieldInteger(arguments, "transport", transport, 0, 2))
        return -EINVAL;
      if (!services.bluetoothPair(
              address, static_cast<network::BluetoothTransport>(transport)))
        return -EIO;
    } else if (method == "bluetooth.unpair") {
      if (!services.bluetoothUnpair(address))
        return -EIO;
    } else if (method == "bluetooth.cancel-pairing") {
      if (!services.bluetoothCancelPairing(address))
        return -EIO;
    } else {
      return -ENOSYS;
    }
    response = "null";
    return 0;
  }

  if (method == "camera.enumerate" || method == "camera.set-torch") {
    if (!hasPermission(context, apps::DeviceServicePermission::Camera))
      return -EACCES;
    if (method == "camera.enumerate") {
      std::vector<hardware::CameraInfo> cameras;
      if (!services.enumerateCameras(cameras))
        return -EIO;
      response = serializeCameras(cameras);
      return 0;
    }
    std::string camera_id;
    bool enabled = false;
    if (!fieldString(arguments, "cameraId", camera_id, 128) ||
        !fieldBoolean(arguments, "enabled", enabled))
      return -EINVAL;
    if (!services.setTorch(camera_id, enabled))
      return -EIO;
    response = "null";
    return 0;
  }

  if (method == "modem.snapshot" || method == "modem.radio-power") {
    if (context->restrict_connectivity)
      return -ENOTSUP;
    if (!hasPermission(context, apps::DeviceServicePermission::Modem))
      return -EACCES;
    int64_t timeout_ms = method == "modem.snapshot" ? 5000 : 10000;
    const apps::JsonValue *timeout = arguments.get("timeoutMs");
    if (timeout &&
        (!timeout->isNumber() || timeout->integerValue() < 0 ||
         timeout->integerValue() > 60000))
      return -EINVAL;
    if (timeout)
      timeout_ms = timeout->integerValue();
    if (method == "modem.snapshot") {
      modem::ModemSnapshot snapshot;
      if (!services.modemSnapshot(snapshot, timeout_ms))
        return -EIO;
      response = serializeModem(snapshot);
      return 0;
    }
    bool enabled = false;
    modem::ModemRequestStatus status;
    if (!fieldBoolean(arguments, "enabled", enabled))
      return -EINVAL;
    if (!services.setRadioPower(enabled, status, timeout_ms))
      return -EIO;
    response = "{";
    appendStringField(response, "operation", status.operation);
    appendIntegerField(response, "error", status.error);
    appendBooleanField(response, "timedOut", status.timed_out);
    response.push_back('}');
    return 0;
  }
  return -ENOSYS;
}

} // namespace

bool serviceDeviceApi(int socket_fd, storage::DeviceStorageService &service,
                      bool &connected, std::string &error, int timeout_ms,
                      DeviceApiContext *context) {
  if (!connected)
    return true;
  OosDeviceApiRequest request = {};
  const int received = oos_device_api_receive(socket_fd, &request, timeout_ms);
  if (received == -ETIMEDOUT)
    return true;
  if (received == 0) {
    connected = false;
    return true;
  }
  if (received < 0) {
    error = errorText("receive WPE device API request", received);
    return false;
  }

  const auto volume = static_cast<storage::DeviceStorageVolume>(request.volume);
  int status = 0;
  const void *payload = nullptr;
  uint32_t payload_size = 0;
  std::string serialized;
  std::vector<uint8_t> bytes;
  uint64_t space_bytes = 0;
  const bool storage_permission =
      !context ||
      hasPermission(context, apps::DeviceServicePermission::DeviceStorage);
  if (request.operation != OOS_DEVICE_API_PLATFORM_CALL &&
      !storage_permission) {
    status = -EACCES;
  } else if (request.operation == OOS_DEVICE_API_LIST_FILES) {
    std::vector<storage::DeviceStorageEntry> entries;
    if (!service.list(volume, entries)) {
      status = -ENOENT;
    } else {
      serialized = serializeEntries(entries);
      payload = serialized.data();
      payload_size = static_cast<uint32_t>(serialized.size());
    }
  } else if (request.operation == OOS_DEVICE_API_READ_FILE) {
    if (!service.read(volume, request.path, bytes)) {
      status = -ENOENT;
    } else {
      payload = bytes.data();
      payload_size = static_cast<uint32_t>(bytes.size());
    }
  } else if (request.operation == OOS_DEVICE_API_WRITE_FILE) {
    if (!service.write(
            volume, request.path,
            static_cast<storage::DeviceStorageWriteMode>(request.flags),
            static_cast<const uint8_t *>(request.payload),
            request.payload_size))
      status = -EIO;
  } else if (request.operation == OOS_DEVICE_API_DELETE_FILE) {
    bool removed = false;
    if (!service.remove(volume, request.path, removed))
      status = -EIO;
  } else if (request.operation == OOS_DEVICE_API_FREE_SPACE ||
             request.operation == OOS_DEVICE_API_USED_SPACE) {
    const bool success = request.operation == OOS_DEVICE_API_FREE_SPACE
                             ? service.freeSpace(volume, space_bytes)
                             : service.usedSpace(volume, space_bytes);
    if (!success) {
      status = -EIO;
    } else {
      payload = &space_bytes;
      payload_size = sizeof(space_bytes);
    }
  } else if (request.operation == OOS_DEVICE_API_PLATFORM_CALL) {
    status = servicePlatformCall(request, context, serialized);
    if (status == 0) {
      payload = serialized.data();
      payload_size = static_cast<uint32_t>(serialized.size());
    }
  } else {
    status = -ENOSYS;
  }
  const int replied = oos_device_api_reply(socket_fd, status, payload,
                                           payload_size, kReplyTimeoutMs);
  oos_device_api_request_clear(&request);
  if (replied != 0) {
    error = errorText("reply to WPE device API request", replied);
    return false;
  }
  return true;
}

} // namespace oos::web
