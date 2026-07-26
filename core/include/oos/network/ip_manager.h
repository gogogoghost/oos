#pragma once

#include <string>

namespace oos::network {

struct IpConfiguration {
  std::string interface_name;
  std::string address;
  unsigned prefix_length = 0;
  std::string gateway;
  std::string dns1;
  std::string dns2;
};

class IpManager {
public:
  explicit IpManager(std::string interface_name = "wlan0");

  bool status(IpConfiguration &configuration);
  bool useDhcp(int timeout_ms = 15000);
  bool useStatic(const IpConfiguration &configuration);
  const std::string &lastError() const;

private:
  bool setServiceState(const char *action, int timeout_ms);

  std::string interface_name_;
  std::string service_name_;
  std::string last_error_;
};

} // namespace oos::network
