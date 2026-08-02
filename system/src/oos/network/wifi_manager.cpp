#include "oos/network/wifi_manager.h"

#include "oos/network/supplicant_text.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>
#include <utility>

namespace oos::network {
namespace {

constexpr char kModernControlSocket[] = "/data/vendor/wifi/wpa/sockets/wlan0";
constexpr char kLegacyControlSocket[] = "/data/misc/wifi/sockets/wlan0";
std::atomic<unsigned> g_client_sequence{0};

std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(std::move(line));
  }
  return lines;
}

std::vector<std::string> splitTabs(const std::string &line) {
  std::vector<std::string> fields;
  size_t begin = 0;
  while (begin <= line.size()) {
    const size_t end = line.find('\t', begin);
    fields.push_back(line.substr(begin, end - begin));
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return fields;
}

bool parseInteger(const std::string &value, int &result) {
  const size_t end_of_value = value.find_last_not_of("\r\n \t");
  if (end_of_value == std::string::npos)
    return false;
  const std::string trimmed = value.substr(0, end_of_value + 1);
  char *end = nullptr;
  errno = 0;
  const long parsed = std::strtol(trimmed.c_str(), &end, 10);
  if (errno || !end || *end)
    return false;
  result = static_cast<int>(parsed);
  return true;
}

bool validField(const std::string &value) {
  return value.find_first_of("\r\n\0", 0, 3) == std::string::npos;
}

std::string hexEncode(const std::string &value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() * 2);
  for (const unsigned char byte : value) {
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 0x0f]);
  }
  return result;
}

bool isHexPsk(const std::string &value) {
  if (value.size() != 64)
    return false;
  return value.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

std::string quotePsk(const std::string &value) {
  std::string result = "\"";
  for (const char character : value) {
    if (character == '\\' || character == '\"')
      result.push_back('\\');
    result.push_back(character);
  }
  result.push_back('\"');
  return result;
}

} // namespace

struct WifiManager::Implementation {
  explicit Implementation(std::string requested_socket)
      : control_socket(std::move(requested_socket)) {}

  bool request(const std::string &command, std::string &response,
               int timeout_ms = 5000) {
    if (fd < 0) {
      error = "Wi-Fi control socket is not initialized";
      return false;
    }
    if (send(fd, command.data(), command.size(), 0) < 0) {
      error = "send " + command.substr(0, command.find(' ')) + ": " +
              std::strerror(errno);
      return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    std::vector<char> buffer(65536);
    while (std::chrono::steady_clock::now() < deadline) {
      const int remaining = static_cast<int>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now())
              .count());
      pollfd ready{fd, POLLIN, 0};
      const int result = poll(&ready, 1, remaining > 0 ? remaining : 1);
      if (result < 0 && errno == EINTR)
        continue;
      if (result <= 0) {
        error = result == 0 ? "Wi-Fi command timed out" : std::strerror(errno);
        return false;
      }
      const ssize_t length = recv(fd, buffer.data(), buffer.size(), 0);
      if (length < 0 && errno == EINTR)
        continue;
      if (length <= 0) {
        error =
            length == 0 ? "Wi-Fi control socket closed" : std::strerror(errno);
        return false;
      }
      response.assign(buffer.data(), static_cast<size_t>(length));
      // Attached supplicant events begin with a priority marker such as <3>.
      if (!response.empty() && response.front() == '<')
        continue;
      if (response.rfind("FAIL", 0) == 0 || response.rfind("UNKNOWN", 0) == 0) {
        error = command.substr(0, command.find(' ')) + " returned " + response;
        return false;
      }
      error.clear();
      return true;
    }
    error = "Wi-Fi command timed out";
    return false;
  }

  std::string control_socket;
  std::string local_socket;
  std::string error;
  int fd = -1;
};

WifiManager::WifiManager(std::string control_socket)
    : implementation_(
          std::make_unique<Implementation>(std::move(control_socket))) {}

WifiManager::~WifiManager() { shutdown(); }

bool WifiManager::initialize() {
  shutdown();
  if (implementation_->control_socket == kModernControlSocket &&
      access(kModernControlSocket, F_OK) != 0 &&
      access(kLegacyControlSocket, F_OK) == 0) {
    implementation_->control_socket = kLegacyControlSocket;
  }
  implementation_->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (implementation_->fd < 0) {
    implementation_->error = std::strerror(errno);
    return false;
  }

  implementation_->local_socket =
      "/dev/socket/oos-wpa-" + std::to_string(getpid()) + "-" +
      std::to_string(g_client_sequence.fetch_add(1, std::memory_order_relaxed));
  unlink(implementation_->local_socket.c_str());
  sockaddr_un local{};
  local.sun_family = AF_UNIX;
  std::snprintf(local.sun_path, sizeof(local.sun_path), "%s",
                implementation_->local_socket.c_str());
  if (bind(implementation_->fd, reinterpret_cast<sockaddr *>(&local),
           sizeof(local)) < 0) {
    implementation_->error =
        "bind Wi-Fi client socket: " + std::string(std::strerror(errno));
    shutdown();
    return false;
  }

  sockaddr_un remote{};
  remote.sun_family = AF_UNIX;
  std::snprintf(remote.sun_path, sizeof(remote.sun_path), "%s",
                implementation_->control_socket.c_str());
  if (::connect(implementation_->fd, reinterpret_cast<sockaddr *>(&remote),
                sizeof(remote)) < 0) {
    implementation_->error = "connect " + implementation_->control_socket +
                             ": " + std::strerror(errno);
    shutdown();
    return false;
  }
  std::string response;
  return implementation_->request("PING", response) &&
         response.rfind("PONG", 0) == 0;
}

void WifiManager::shutdown() {
  if (implementation_->fd >= 0) {
    close(implementation_->fd);
    implementation_->fd = -1;
  }
  if (!implementation_->local_socket.empty()) {
    unlink(implementation_->local_socket.c_str());
    implementation_->local_socket.clear();
  }
}

bool WifiManager::initialized() const { return implementation_->fd >= 0; }

bool WifiManager::status(WifiStatus &status_value) {
  std::string response;
  if (!implementation_->request("STATUS", response))
    return false;
  status_value = {};
  for (const std::string &line : splitLines(response)) {
    const size_t separator = line.find('=');
    if (separator == std::string::npos)
      continue;
    const std::string key = line.substr(0, separator);
    const std::string value = line.substr(separator + 1);
    if (key == "wpa_state")
      status_value.state = value;
    else if (key == "ssid")
      status_value.ssid = decodeSupplicantText(value);
    else if (key == "bssid")
      status_value.bssid = value;
    else if (key == "ip_address")
      status_value.ip_address = value;
    else if (key == "id")
      parseInteger(value, status_value.network_id);
  }
  return true;
}

bool WifiManager::scan(std::vector<WifiAccessPoint> &results, int wait_ms) {
  std::string response;
  if (!implementation_->request("SCAN", response))
    return false;
  std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
  if (!implementation_->request("SCAN_RESULTS", response))
    return false;
  results.clear();
  const auto lines = splitLines(response);
  for (size_t index = 1; index < lines.size(); ++index) {
    const auto fields = splitTabs(lines[index]);
    if (fields.size() < 5)
      continue;
    WifiAccessPoint access_point;
    access_point.bssid = fields[0];
    parseInteger(fields[1], access_point.frequency_mhz);
    parseInteger(fields[2], access_point.signal_dbm);
    access_point.flags = fields[3];
    access_point.ssid = decodeSupplicantText(fields[4]);
    results.push_back(std::move(access_point));
  }
  return true;
}

bool WifiManager::listNetworks(std::vector<WifiNetwork> &networks) {
  std::string response;
  if (!implementation_->request("LIST_NETWORKS", response))
    return false;
  networks.clear();
  const auto lines = splitLines(response);
  for (size_t index = 1; index < lines.size(); ++index) {
    const auto fields = splitTabs(lines[index]);
    if (fields.size() < 4)
      continue;
    WifiNetwork network;
    if (!parseInteger(fields[0], network.id))
      continue;
    network.ssid = decodeSupplicantText(fields[1]);
    network.bssid = fields[2];
    network.flags = fields[3];
    networks.push_back(std::move(network));
  }
  return true;
}

bool WifiManager::connect(const std::string &ssid, WifiSecurity security,
                          const std::string &credential, int &network_id) {
  if (ssid.empty() || ssid.size() > 32 || !validField(ssid) ||
      !validField(credential)) {
    implementation_->error = "invalid SSID or credential";
    return false;
  }
  if (security == WifiSecurity::WpaPsk &&
      (credential.size() < 8 || credential.size() > 64)) {
    implementation_->error =
        "WPA-PSK must contain 8-63 characters or 64 hex digits";
    return false;
  }

  std::string response;
  if (!implementation_->request("ADD_NETWORK", response) ||
      !parseInteger(response, network_id)) {
    if (implementation_->error.empty())
      implementation_->error = "invalid ADD_NETWORK response";
    return false;
  }
  const std::string id = std::to_string(network_id);
  auto set = [&](const std::string &field, const std::string &value) {
    return implementation_->request(
        "SET_NETWORK " + id + " " + field + " " + value, response);
  };
  bool configured = set("ssid", hexEncode(ssid));
  if (configured && security == WifiSecurity::Open)
    configured = set("key_mgmt", "NONE");
  if (configured && security == WifiSecurity::WpaPsk) {
    configured =
        set("key_mgmt", "WPA-PSK") &&
        set("psk", isHexPsk(credential) ? credential : quotePsk(credential));
  }
  if (configured)
    configured = implementation_->request("ENABLE_NETWORK " + id, response) &&
                 implementation_->request("SELECT_NETWORK " + id, response);
  if (configured)
    return true;
  std::string ignored;
  implementation_->request("REMOVE_NETWORK " + id, ignored);
  return false;
}

bool WifiManager::select(int network_id) {
  if (network_id < 0) {
    implementation_->error = "invalid Wi-Fi network ID";
    return false;
  }
  std::string response;
  const std::string id = std::to_string(network_id);
  return implementation_->request("ENABLE_NETWORK " + id, response) &&
         implementation_->request("SELECT_NETWORK " + id, response);
}

bool WifiManager::disconnect() {
  std::string response;
  return implementation_->request("DISCONNECT", response);
}

bool WifiManager::reconnect() {
  std::string response;
  return implementation_->request("RECONNECT", response);
}

bool WifiManager::forget(int network_id) {
  std::string response;
  return implementation_->request(
      "REMOVE_NETWORK " + std::to_string(network_id), response);
}

bool WifiManager::saveConfiguration() {
  std::string response;
  return implementation_->request("SAVE_CONFIG", response);
}

const std::string &WifiManager::lastError() const {
  return implementation_->error;
}

} // namespace oos::network
