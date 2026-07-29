#pragma once

#include <string>

namespace oos::storage {
class DeviceStorageService;
}

namespace oos::web {

// Services at most one request. A timeout is not an error; disconnected is
// cleared when the peer closes the private control socket.
bool serviceDeviceApi(int socket_fd, storage::DeviceStorageService &service,
                      bool &connected, std::string &error, int timeout_ms = 0);

} // namespace oos::web
