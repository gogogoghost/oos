#pragma once

#include <memory>
#include <string>
#include <vector>

namespace oos::network {

struct WifiStatus {
  std::string state;
  std::string ssid;
  std::string bssid;
  std::string ip_address;
  int network_id = -1;
};

struct WifiAccessPoint {
  std::string bssid;
  int frequency_mhz = 0;
  int signal_dbm = 0;
  std::string flags;
  std::string ssid;
};

struct WifiNetwork {
  int id = -1;
  std::string ssid;
  std::string bssid;
  std::string flags;
};

enum class WifiSecurity { Open, WpaPsk };

class WifiManager {
public:
  explicit WifiManager(
      std::string control_socket = "/data/vendor/wifi/wpa/sockets/wlan0");
  ~WifiManager();

  WifiManager(const WifiManager &) = delete;
  WifiManager &operator=(const WifiManager &) = delete;

  bool initialize();
  void shutdown();
  bool initialized() const;

  bool status(WifiStatus &status);
  bool scan(std::vector<WifiAccessPoint> &results, int wait_ms = 3000);
  bool listNetworks(std::vector<WifiNetwork> &networks);
  bool connect(const std::string &ssid, WifiSecurity security,
               const std::string &credential, int &network_id);
  bool disconnect();
  bool reconnect();
  bool forget(int network_id);
  bool saveConfiguration();
  const std::string &lastError() const;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace oos::network
