#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace oos::device {
class Device;
class ServiceProvider;
}

namespace oos::storage {
class AppStorage;
class DeviceStorageService;
}

namespace oos::web {

struct DeviceApiContext {
  device::ServiceProvider *services = nullptr;
  const device::Device *device = nullptr;
  storage::AppStorage *app_storage = nullptr;
  uint32_t permission_mask = 0;
  std::unordered_map<std::string, bool> owned_data_stores;
  std::vector<std::string> wake_locks;
};

// Services at most one request. A timeout is not an error; disconnected is
// cleared when the peer closes the private control socket.
bool serviceDeviceApi(int socket_fd, storage::DeviceStorageService &service,
                      bool &connected, std::string &error, int timeout_ms = 0,
                      DeviceApiContext *context = nullptr);

} // namespace oos::web
