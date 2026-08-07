#include "oos/device/service_provider.h"

#include "app.h"

namespace oos::device {
namespace {

std::string copy(const app_string_t &value) {
  return std::string(value.ptr ? reinterpret_cast<const char *>(value.ptr) : "",
                     value.len);
}

app_string_t view(const std::string &value) {
  return {reinterpret_cast<uint8_t *>(const_cast<char *>(value.data())),
          value.size()};
}

} // namespace

struct ServiceProvider::Impl {
  std::string error;
};

ServiceProvider::ServiceProvider(const Device &)
    : impl_(std::make_unique<Impl>()) {}
ServiceProvider::~ServiceProvider() = default;

bool ServiceProvider::wifiStatus(network::WifiStatus &status) {
  oos_platform_wifi_status_t value{};
  oos_platform_wifi_error_code_t error{};
  if (!oos_platform_wifi_get_status(&value, &error)) {
    impl_->error = "read Wi-Fi status failed";
    return false;
  }
  status = {copy(value.state), copy(value.ssid), copy(value.bssid),
            copy(value.ip_address), value.network_id};
  oos_platform_wifi_status_free(&value);
  impl_->error.clear();
  return true;
}

bool ServiceProvider::wifiEnabled(bool &enabled) {
  oos_platform_wifi_error_code_t error{};
  if (!oos_platform_wifi_enabled(&enabled, &error)) {
    impl_->error = "read Wi-Fi power state failed";
    return false;
  }
  impl_->error.clear();
  return true;
}

bool ServiceProvider::wifiSetEnabled(bool enabled) {
  oos_platform_wifi_error_code_t error{};
  const bool result = oos_platform_wifi_set_enabled(enabled, &error);
  impl_->error = result ? "" : "change Wi-Fi power state failed";
  return result;
}

bool ServiceProvider::wifiScan(std::vector<network::WifiAccessPoint> &results,
                               int wait_ms) {
  oos_platform_wifi_list_access_point_t values{};
  oos_platform_wifi_error_code_t error{};
  if (wait_ms < 0 ||
      !oos_platform_wifi_scan(static_cast<uint32_t>(wait_ms), &values,
                              &error)) {
    impl_->error = "scan Wi-Fi networks failed";
    return false;
  }
  results.clear();
  results.reserve(values.len);
  for (size_t index = 0; index < values.len; ++index) {
    const auto &value = values.ptr[index];
    results.push_back({copy(value.bssid), value.frequency_mhz, value.signal_dbm,
                       copy(value.capabilities), copy(value.ssid)});
  }
  oos_platform_wifi_list_access_point_free(&values);
  impl_->error.clear();
  return true;
}

bool ServiceProvider::wifiListNetworks(
    std::vector<network::WifiNetwork> &networks) {
  oos_platform_wifi_list_network_t values{};
  oos_platform_wifi_error_code_t error{};
  if (!oos_platform_wifi_list_networks(&values, &error)) {
    impl_->error = "list saved Wi-Fi networks failed";
    return false;
  }
  networks.clear();
  networks.reserve(values.len);
  for (size_t index = 0; index < values.len; ++index) {
    const auto &value = values.ptr[index];
    networks.push_back({value.id, copy(value.ssid), copy(value.bssid),
                        copy(value.capabilities)});
  }
  oos_platform_wifi_list_network_free(&values);
  impl_->error.clear();
  return true;
}

bool ServiceProvider::wifiConnect(const std::string &ssid,
                                  network::WifiSecurity security,
                                  const std::string &credential,
                                  int &network_id) {
  app_string_t native_ssid = view(ssid);
  app_string_t native_credential = view(credential);
  oos_platform_wifi_error_code_t error{};
  const bool result = oos_platform_wifi_connect(
      &native_ssid, static_cast<oos_platform_wifi_security_t>(security),
      &native_credential, &network_id, &error);
  impl_->error = result ? "" : "connect Wi-Fi network failed";
  return result;
}

bool ServiceProvider::wifiSelect(int network_id) {
  oos_platform_wifi_error_code_t error{};
  const bool result = oos_platform_wifi_select(network_id, &error);
  impl_->error = result ? "" : "select Wi-Fi network failed";
  return result;
}

bool ServiceProvider::wifiDisconnect() {
  oos_platform_wifi_error_code_t error{};
  const bool result = oos_platform_wifi_disconnect(&error);
  impl_->error = result ? "" : "disconnect Wi-Fi failed";
  return result;
}

bool ServiceProvider::wifiReconnect() {
  oos_platform_wifi_error_code_t error{};
  const bool result = oos_platform_wifi_reconnect(&error);
  impl_->error = result ? "" : "reconnect Wi-Fi failed";
  return result;
}

bool ServiceProvider::wifiForget(int network_id) {
  oos_platform_wifi_error_code_t error{};
  const bool result = oos_platform_wifi_forget(network_id, &error);
  impl_->error = result ? "" : "forget Wi-Fi network failed";
  return result;
}

bool ServiceProvider::wifiSaveConfiguration() {
  oos_platform_wifi_error_code_t error{};
  const bool result = oos_platform_wifi_save_configuration(&error);
  impl_->error = result ? "" : "save Wi-Fi configuration failed";
  return result;
}

bool ServiceProvider::ipUseDhcp(int timeout_ms) {
  oos_platform_ip_error_code_t error{};
  const bool result = timeout_ms >= 0 &&
                      oos_platform_ip_use_dhcp(
                          static_cast<uint32_t>(timeout_ms), &error);
  impl_->error = result ? "" : "configure DHCP failed";
  return result;
}

std::string ServiceProvider::lastError() const { return impl_->error; }

} // namespace oos::device
