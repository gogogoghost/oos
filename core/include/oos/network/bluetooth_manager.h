#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oos::network {

enum class BluetoothTransport : uint8_t {
  Auto = 0,
  Classic = 1,
  LowEnergy = 2
};

enum class BluetoothProfile : uint8_t {
  Hid = 0x03,
  HandsFree = 0x05,
  A2dp = 0x06,
};

struct BluetoothDevice {
  std::string address;
  std::string name;
  int rssi = 0;
  uint32_t device_class = 0;
  int device_type = 0;
  std::vector<uint8_t> advertising_data;
};

class BluetoothManager {
public:
  BluetoothManager();
  ~BluetoothManager();

  BluetoothManager(const BluetoothManager &) = delete;
  BluetoothManager &operator=(const BluetoothManager &) = delete;

  // Owns the stock bluetoothd instance until shutdown().
  bool initialize(const std::string &service_name = "bluetoothd_socket1");
  void shutdown();
  bool initialized() const;

  bool enable(int timeout_ms = 10000);
  bool disable(int timeout_ms = 10000);
  bool classicScan(std::vector<BluetoothDevice> &devices, int duration_ms);
  bool leScan(std::vector<BluetoothDevice> &devices, int duration_ms);
  bool pair(const std::string &address, BluetoothTransport transport);
  bool unpair(const std::string &address);
  bool cancelPairing(const std::string &address);
  // Profile operations are asynchronous: true means bluetoothd accepted the
  // request. Connection-state notifications belong in the production event
  // loop; profileConnectionCycle keeps this diagnostic process alive between
  // an explicit connect and disconnect request.
  bool profileConnect(const std::string &address, BluetoothProfile profile);
  bool profileDisconnect(const std::string &address, BluetoothProfile profile);
  bool profileConnectionCycle(const std::string &address,
                              BluetoothProfile profile, int hold_ms);
  // Registers a GATT client, establishes a direct LE link, then disconnects
  // and unregisters it. Use an address owned by the tester.
  bool leConnectionCycle(const std::string &address, int hold_ms,
                         int timeout_ms = 15000);

  const std::string &lastError() const;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace oos::network
