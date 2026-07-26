#include "oos/network/bluetooth_manager.h"

#include <cutils/properties.h>
#include <poll.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>

namespace oos::network {
namespace {

constexpr uint8_t kSetupService = 0x00;
constexpr uint8_t kCoreService = 0x01;
constexpr uint8_t kGattService = 0x09;
constexpr uint8_t kRegisterModule = 0x01;
constexpr uint8_t kUnregisterModule = 0x02;
constexpr uint8_t kEnable = 0x01;
constexpr uint8_t kDisable = 0x02;
constexpr uint8_t kStartDiscovery = 0x0b;
constexpr uint8_t kCancelDiscovery = 0x0c;
constexpr uint8_t kCreateBond = 0x0d;
constexpr uint8_t kRemoveBond = 0x0e;
constexpr uint8_t kCancelBond = 0x0f;
constexpr uint8_t kAdapterStateNotification = 0x81;
constexpr uint8_t kDeviceFoundNotification = 0x84;
constexpr uint8_t kRegisterScanner = 0x41;
constexpr uint8_t kUnregisterScanner = 0x42;
constexpr uint8_t kScan = 0x43;
constexpr uint8_t kRegisterScannerNotification = 0xc1;
constexpr uint8_t kScanResultNotification = 0xcd;
constexpr uint8_t kGattRegisterClient = 0x01;
constexpr uint8_t kGattUnregisterClient = 0x02;
constexpr uint8_t kGattConnect = 0x03;
constexpr uint8_t kGattDisconnect = 0x04;
constexpr uint8_t kGattRegisterClientNotification = 0x81;
constexpr uint8_t kGattConnectNotification = 0x82;
constexpr uint8_t kGattDisconnectNotification = 0x83;

struct Pdu {
  uint8_t service = 0;
  uint8_t opcode = 0;
  std::vector<uint8_t> payload;
};

template <typename T> void append(std::vector<uint8_t> &data, T value) {
  for (size_t index = 0; index < sizeof(value); ++index)
    data.push_back(static_cast<uint8_t>(value >> (index * 8)));
}

template <typename T>
bool read(const std::vector<uint8_t> &data, size_t &offset, T &value) {
  if (offset + sizeof(value) > data.size())
    return false;
  value = 0;
  for (size_t index = 0; index < sizeof(value); ++index)
    value |= static_cast<T>(data[offset++]) << (index * 8);
  return true;
}

std::string addressText(const uint8_t *address) {
  char text[18]{};
  std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", address[0],
                address[1], address[2], address[3], address[4], address[5]);
  return text;
}

bool parseAddress(const std::string &text, std::array<uint8_t, 6> &address) {
  unsigned values[6]{};
  char tail = 0;
  if (std::sscanf(text.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x%c", &values[0],
                  &values[1], &values[2], &values[3], &values[4], &values[5],
                  &tail) != 6)
    return false;
  for (size_t index = 0; index < address.size(); ++index)
    address[index] = static_cast<uint8_t>(values[index]);
  return true;
}

void mergeDevice(std::vector<BluetoothDevice> &devices,
                 BluetoothDevice candidate) {
  auto existing = std::find_if(devices.begin(), devices.end(),
                               [&](const BluetoothDevice &device) {
                                 return device.address == candidate.address;
                               });
  if (existing == devices.end()) {
    devices.push_back(std::move(candidate));
    return;
  }
  if (!candidate.name.empty())
    existing->name = std::move(candidate.name);
  if (candidate.rssi)
    existing->rssi = candidate.rssi;
  if (candidate.device_class)
    existing->device_class = candidate.device_class;
  if (candidate.device_type)
    existing->device_type = candidate.device_type;
  if (!candidate.advertising_data.empty())
    existing->advertising_data = std::move(candidate.advertising_data);
}

} // namespace

struct BluetoothManager::Implementation {
  bool sendPdu(int fd, uint8_t service, uint8_t opcode,
               const std::vector<uint8_t> &payload) {
    if (payload.size() > UINT16_MAX) {
      error = "Bluetooth PDU is too large";
      return false;
    }
    std::vector<uint8_t> packet;
    packet.reserve(payload.size() + 4);
    packet.push_back(service);
    packet.push_back(opcode);
    append<uint16_t>(packet, static_cast<uint16_t>(payload.size()));
    packet.insert(packet.end(), payload.begin(), payload.end());
    if (send(fd, packet.data(), packet.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(packet.size())) {
      error = "send Bluetooth PDU: " + std::string(std::strerror(errno));
      return false;
    }
    return true;
  }

  bool receivePdu(int fd, Pdu &pdu, int timeout_ms) {
    pollfd ready{fd, POLLIN, 0};
    int result;
    do {
      result = poll(&ready, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result <= 0) {
      if (result < 0)
        error = "poll Bluetooth PDU: " + std::string(std::strerror(errno));
      return false;
    }
    std::vector<uint8_t> packet(65539);
    const ssize_t length = recv(fd, packet.data(), packet.size(), 0);
    if (length < 4) {
      error = length < 0 ? std::strerror(errno) : "short Bluetooth PDU";
      return false;
    }
    const uint16_t payload_length =
        static_cast<uint16_t>(packet[2] | (packet[3] << 8));
    if (length != static_cast<ssize_t>(payload_length + 4)) {
      error = "invalid Bluetooth PDU length";
      return false;
    }
    pdu.service = packet[0];
    pdu.opcode = packet[1];
    pdu.payload.assign(packet.begin() + 4, packet.begin() + length);
    return true;
  }

  bool command(uint8_t service, uint8_t opcode,
               const std::vector<uint8_t> &payload = {},
               int timeout_ms = 5000) {
    if (!sendPdu(command_fd, service, opcode, payload))
      return false;
    Pdu response;
    if (!receivePdu(command_fd, response, timeout_ms)) {
      if (error.empty())
        error = "Bluetooth command response timed out";
      return false;
    }
    if (response.service != service) {
      error = "unexpected Bluetooth response service";
      return false;
    }
    if (response.opcode == 0) {
      error = "Bluetooth command failed";
      if (!response.payload.empty())
        error += " with status " + std::to_string(response.payload[0]);
      return false;
    }
    if (response.opcode != opcode) {
      error = "unexpected Bluetooth response opcode";
      return false;
    }
    error.clear();
    return true;
  }

  bool registerModule(uint8_t module) {
    std::vector<uint8_t> payload{module, 0};
    append<uint32_t>(payload, 1);
    return command(kSetupService, kRegisterModule, payload);
  }

  bool unregisterModule(uint8_t module) {
    return command(kSetupService, kUnregisterModule, {module});
  }

  bool ensureModule(uint8_t module) {
    if (std::find(registered_modules.begin(), registered_modules.end(),
                  module) != registered_modules.end())
      return true;
    if (!registerModule(module))
      return false;
    registered_modules.push_back(module);
    return true;
  }

  bool releaseModule(uint8_t module) {
    const auto entry =
        std::find(registered_modules.begin(), registered_modules.end(), module);
    if (entry == registered_modules.end())
      return true;
    if (!unregisterModule(module))
      return false;
    registered_modules.erase(entry);
    return true;
  }

  bool waitAdapterState(bool enabled, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      const int remaining = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now())
              .count());
      Pdu notification;
      if (!receivePdu(notification_fd, notification, remaining))
        break;
      if (notification.service == kCoreService &&
          notification.opcode == kAdapterStateNotification &&
          !notification.payload.empty() &&
          static_cast<bool>(notification.payload[0]) == enabled)
        return true;
    }
    error = "Bluetooth adapter state notification timed out";
    return false;
  }

  static bool parseClassicDevice(const Pdu &pdu, BluetoothDevice &device) {
    if (pdu.payload.empty())
      return false;
    size_t offset = 1;
    for (uint8_t index = 0; index < pdu.payload[0]; ++index) {
      uint8_t type = 0;
      uint16_t length = 0;
      if (!read(pdu.payload, offset, type) ||
          !read(pdu.payload, offset, length) ||
          offset + length > pdu.payload.size())
        return false;
      const uint8_t *value = pdu.payload.data() + offset;
      if (type == 1)
        device.name.assign(reinterpret_cast<const char *>(value), length);
      else if (type == 2 && length == 6)
        device.address = addressText(value);
      else if (type == 4 && length == 4) {
        size_t property_offset = offset;
        read(pdu.payload, property_offset, device.device_class);
      } else if (type == 5 && length == 4) {
        uint32_t device_type = 0;
        size_t property_offset = offset;
        read(pdu.payload, property_offset, device_type);
        device.device_type = static_cast<int>(device_type);
      } else if (type == 11 && length == 1) {
        device.rssi = static_cast<int8_t>(*value);
      }
      offset += length;
    }
    return !device.address.empty();
  }

  std::string service_name;
  std::string error;
  int listen_fd = -1;
  int command_fd = -1;
  int notification_fd = -1;
  bool core_registered = false;
  bool adapter_enabled = false;
  bool enabled_by_us = false;
  std::vector<uint8_t> registered_modules;
};

BluetoothManager::BluetoothManager()
    : implementation_(std::make_unique<Implementation>()) {}

BluetoothManager::~BluetoothManager() { shutdown(); }

bool BluetoothManager::initialize(const std::string &service_name) {
  shutdown();
  if (service_name.empty() ||
      service_name.size() + 2 > sizeof(sockaddr_un::sun_path)) {
    implementation_->error = "invalid bluetoothd service name";
    return false;
  }
  implementation_->service_name = service_name;
  implementation_->listen_fd =
      socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (implementation_->listen_fd < 0) {
    implementation_->error = std::strerror(errno);
    return false;
  }
  int reuse = 1;
  setsockopt(implementation_->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
             sizeof(reuse));
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  address.sun_path[0] = '\0';
  std::memcpy(address.sun_path + 1, service_name.c_str(),
              service_name.size() + 1);
  const socklen_t address_length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + service_name.size() + 2);
  if (bind(implementation_->listen_fd, reinterpret_cast<sockaddr *>(&address),
           address_length) < 0 ||
      listen(implementation_->listen_fd, 2) < 0) {
    implementation_->error =
        "listen for bluetoothd: " + std::string(std::strerror(errno));
    shutdown();
    return false;
  }
  if (property_set("ctl.start", service_name.c_str()) < 0) {
    implementation_->error = "could not start bluetoothd";
    shutdown();
    return false;
  }
  auto acceptOne = [&]() -> int {
    pollfd ready{implementation_->listen_fd, POLLIN, 0};
    int result;
    do {
      result = poll(&ready, 1, 10000);
    } while (result < 0 && errno == EINTR);
    return result > 0 ? accept4(implementation_->listen_fd, nullptr, nullptr,
                                SOCK_CLOEXEC)
                      : -1;
  };
  implementation_->command_fd = acceptOne();
  implementation_->notification_fd = acceptOne();
  if (implementation_->command_fd < 0 || implementation_->notification_fd < 0) {
    implementation_->error = "bluetoothd did not connect both IPC channels";
    shutdown();
    return false;
  }
  if (!implementation_->registerModule(kCoreService)) {
    shutdown();
    return false;
  }
  implementation_->core_registered = true;
  return true;
}

void BluetoothManager::shutdown() {
  if (!implementation_)
    return;
  if (implementation_->adapter_enabled && implementation_->enabled_by_us) {
    implementation_->command(kCoreService, kDisable);
    implementation_->waitAdapterState(false, 5000);
  }
  while (!implementation_->registered_modules.empty()) {
    implementation_->unregisterModule(
        implementation_->registered_modules.back());
    implementation_->registered_modules.pop_back();
  }
  if (implementation_->core_registered)
    implementation_->unregisterModule(kCoreService);
  implementation_->core_registered = false;
  for (int *fd : {&implementation_->notification_fd,
                  &implementation_->command_fd, &implementation_->listen_fd}) {
    if (*fd >= 0) {
      close(*fd);
      *fd = -1;
    }
  }
  if (!implementation_->service_name.empty()) {
    property_set("ctl.stop", implementation_->service_name.c_str());
    implementation_->service_name.clear();
  }
  implementation_->adapter_enabled = false;
  implementation_->enabled_by_us = false;
}

bool BluetoothManager::initialized() const {
  return implementation_->command_fd >= 0 &&
         implementation_->notification_fd >= 0;
}

bool BluetoothManager::enable(int timeout_ms) {
  if (implementation_->adapter_enabled)
    return true;
  if (!implementation_->command(kCoreService, kEnable) ||
      !implementation_->waitAdapterState(true, timeout_ms))
    return false;
  implementation_->adapter_enabled = true;
  implementation_->enabled_by_us = true;
  return true;
}

bool BluetoothManager::disable(int timeout_ms) {
  if (!implementation_->adapter_enabled)
    return true;
  if (!implementation_->command(kCoreService, kDisable) ||
      !implementation_->waitAdapterState(false, timeout_ms))
    return false;
  implementation_->adapter_enabled = false;
  implementation_->enabled_by_us = false;
  return true;
}

bool BluetoothManager::classicScan(std::vector<BluetoothDevice> &devices,
                                   int duration_ms) {
  devices.clear();
  if (!enable() || !implementation_->command(kCoreService, kStartDiscovery))
    return false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const int remaining =
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count());
    Pdu notification;
    if (!implementation_->receivePdu(implementation_->notification_fd,
                                     notification, remaining))
      break;
    if (notification.service == kCoreService &&
        notification.opcode == kDeviceFoundNotification) {
      BluetoothDevice device;
      if (Implementation::parseClassicDevice(notification, device))
        mergeDevice(devices, std::move(device));
    }
  }
  const bool cancelled =
      implementation_->command(kCoreService, kCancelDiscovery);
  if (!cancelled)
    return false;
  implementation_->error.clear();
  return true;
}

bool BluetoothManager::leScan(std::vector<BluetoothDevice> &devices,
                              int duration_ms) {
  devices.clear();
  if (!enable() || !implementation_->ensureModule(kGattService))
    return false;
  const std::vector<uint8_t> scan_uuid = {0x6f, 0x6f, 0x73, 0x2d, 0x73, 0x63,
                                          0x61, 0x6e, 0x2d, 0x32, 0x37, 0x38,
                                          0x30, 0x00, 0x00, 0x01};
  if (!implementation_->command(kGattService, kRegisterScanner, scan_uuid)) {
    implementation_->releaseModule(kGattService);
    return false;
  }
  int scanner_id = -1;
  const auto register_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < register_deadline) {
    Pdu notification;
    if (!implementation_->receivePdu(implementation_->notification_fd,
                                     notification, 1000))
      continue;
    if (notification.service == kGattService &&
        notification.opcode == kRegisterScannerNotification &&
        notification.payload.size() >= 21) {
      size_t offset = 0;
      uint32_t status = 1;
      read(notification.payload, offset, status);
      scanner_id = notification.payload[offset];
      if (status != 0)
        scanner_id = -1;
      break;
    }
  }
  if (scanner_id < 0 || !implementation_->command(kGattService, kScan, {1})) {
    implementation_->error = "BLE scanner registration failed";
    implementation_->releaseModule(kGattService);
    return false;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const int remaining =
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count());
    Pdu notification;
    if (!implementation_->receivePdu(implementation_->notification_fd,
                                     notification, remaining))
      break;
    if (notification.service != kGattService ||
        notification.opcode != kScanResultNotification ||
        notification.payload.size() < 12)
      continue;
    BluetoothDevice device;
    device.address = addressText(notification.payload.data());
    size_t offset = 6;
    uint32_t rssi = 0;
    uint16_t length = 0;
    if (!read(notification.payload, offset, rssi) ||
        !read(notification.payload, offset, length) ||
        offset + length > notification.payload.size())
      continue;
    device.rssi = static_cast<int32_t>(rssi);
    device.advertising_data.assign(notification.payload.begin() + offset,
                                   notification.payload.begin() + offset +
                                       length);
    mergeDevice(devices, std::move(device));
  }
  bool ok = implementation_->command(kGattService, kScan, {0});
  std::vector<uint8_t> scanner_payload;
  append<int32_t>(scanner_payload, scanner_id);
  ok = implementation_->command(kGattService, kUnregisterScanner,
                                scanner_payload) &&
       ok;
  ok = implementation_->releaseModule(kGattService) && ok;
  if (ok)
    implementation_->error.clear();
  return ok;
}

bool BluetoothManager::pair(const std::string &address,
                            BluetoothTransport transport) {
  std::array<uint8_t, 6> bytes{};
  if (!parseAddress(address, bytes)) {
    implementation_->error = "invalid Bluetooth address";
    return false;
  }
  std::vector<uint8_t> payload(bytes.begin(), bytes.end());
  payload.push_back(static_cast<uint8_t>(transport));
  return enable() &&
         implementation_->command(kCoreService, kCreateBond, payload);
}

bool BluetoothManager::unpair(const std::string &address) {
  std::array<uint8_t, 6> bytes{};
  if (!parseAddress(address, bytes)) {
    implementation_->error = "invalid Bluetooth address";
    return false;
  }
  return enable() && implementation_->command(
                         kCoreService, kRemoveBond,
                         std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

bool BluetoothManager::cancelPairing(const std::string &address) {
  std::array<uint8_t, 6> bytes{};
  if (!parseAddress(address, bytes)) {
    implementation_->error = "invalid Bluetooth address";
    return false;
  }
  return implementation_->command(
      kCoreService, kCancelBond,
      std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

bool BluetoothManager::profileConnect(const std::string &address,
                                      BluetoothProfile profile) {
  std::array<uint8_t, 6> bytes{};
  const uint8_t service = static_cast<uint8_t>(profile);
  if (!parseAddress(address, bytes) ||
      (profile != BluetoothProfile::Hid &&
       profile != BluetoothProfile::HandsFree &&
       profile != BluetoothProfile::A2dp)) {
    implementation_->error = "invalid Bluetooth address or profile";
    return false;
  }
  return enable() && implementation_->ensureModule(service) &&
         implementation_->command(
             service, 0x01, std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

bool BluetoothManager::profileDisconnect(const std::string &address,
                                         BluetoothProfile profile) {
  std::array<uint8_t, 6> bytes{};
  const uint8_t service = static_cast<uint8_t>(profile);
  if (!parseAddress(address, bytes)) {
    implementation_->error = "invalid Bluetooth address";
    return false;
  }
  return implementation_->command(
      service, 0x02, std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

bool BluetoothManager::profileConnectionCycle(const std::string &address,
                                              BluetoothProfile profile,
                                              int hold_ms) {
  if (!profileConnect(address, profile))
    return false;
  std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
  const bool disconnected = profileDisconnect(address, profile);
  const bool released =
      implementation_->releaseModule(static_cast<uint8_t>(profile));
  return disconnected && released;
}

bool BluetoothManager::leConnectionCycle(const std::string &address,
                                         int hold_ms, int timeout_ms) {
  std::array<uint8_t, 6> bytes{};
  if (!parseAddress(address, bytes)) {
    implementation_->error = "invalid Bluetooth address";
    return false;
  }
  if (!enable() || !implementation_->ensureModule(kGattService))
    return false;
  const std::vector<uint8_t> client_uuid = {0x6f, 0x6f, 0x73, 0x2d, 0x67, 0x61,
                                            0x74, 0x74, 0x2d, 0x32, 0x37, 0x38,
                                            0x30, 0x00, 0x00, 0x01};
  if (!implementation_->command(kGattService, kGattRegisterClient, client_uuid))
    return false;

  int32_t client_id = -1;
  const auto registration_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < registration_deadline) {
    Pdu notification;
    if (!implementation_->receivePdu(implementation_->notification_fd,
                                     notification, 1000))
      continue;
    if (notification.service != kGattService ||
        notification.opcode != kGattRegisterClientNotification ||
        notification.payload.size() < 24)
      continue;
    size_t offset = 0;
    uint32_t status = 1;
    uint32_t raw_client_id = 0;
    read(notification.payload, offset, status);
    read(notification.payload, offset, raw_client_id);
    if (status == 0)
      client_id = static_cast<int32_t>(raw_client_id);
    break;
  }
  if (client_id < 0) {
    implementation_->error = "GATT client registration failed";
    return false;
  }

  std::vector<uint8_t> connect_payload;
  append<int32_t>(connect_payload, client_id);
  connect_payload.insert(connect_payload.end(), bytes.begin(), bytes.end());
  connect_payload.push_back(1);
  append<int32_t>(connect_payload,
                  static_cast<int32_t>(BluetoothTransport::LowEnergy));
  if (!implementation_->command(kGattService, kGattConnect, connect_payload))
    return false;

  int32_t connection_id = -1;
  const auto connection_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < connection_deadline) {
    Pdu notification;
    if (!implementation_->receivePdu(implementation_->notification_fd,
                                     notification, 1000))
      continue;
    if (notification.service != kGattService ||
        notification.opcode != kGattConnectNotification ||
        notification.payload.size() < 18)
      continue;
    size_t offset = 0;
    uint32_t raw_connection_id = 0;
    uint32_t status = 1;
    uint32_t reported_client_id = 0;
    read(notification.payload, offset, raw_connection_id);
    read(notification.payload, offset, status);
    read(notification.payload, offset, reported_client_id);
    if (status == 0 && static_cast<int32_t>(reported_client_id) == client_id)
      connection_id = static_cast<int32_t>(raw_connection_id);
    break;
  }
  bool ok = connection_id >= 0;
  if (!ok)
    implementation_->error = "direct BLE connection failed or timed out";
  if (ok) {
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    std::vector<uint8_t> disconnect_payload;
    append<int32_t>(disconnect_payload, client_id);
    disconnect_payload.insert(disconnect_payload.end(), bytes.begin(),
                              bytes.end());
    append<int32_t>(disconnect_payload, connection_id);
    ok = implementation_->command(kGattService, kGattDisconnect,
                                  disconnect_payload);
    if (ok) {
      const auto disconnect_deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (std::chrono::steady_clock::now() < disconnect_deadline) {
        Pdu notification;
        if (!implementation_->receivePdu(implementation_->notification_fd,
                                         notification, 500))
          continue;
        if (notification.service == kGattService &&
            notification.opcode == kGattDisconnectNotification)
          break;
      }
    }
  }
  std::vector<uint8_t> unregister_payload;
  append<int32_t>(unregister_payload, client_id);
  const bool unregistered = implementation_->command(
      kGattService, kGattUnregisterClient, unregister_payload);
  const bool released = implementation_->releaseModule(kGattService);
  return ok && unregistered && released;
}

const std::string &BluetoothManager::lastError() const {
  return implementation_->error;
}

} // namespace oos::network
