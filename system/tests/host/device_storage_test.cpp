#include "oos/storage/device_storage.h"
#include "oos/web/device_api_transport.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

void writeFile(const std::string &path, const char *contents) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  assert(fd >= 0);
  const size_t size = std::strlen(contents);
  assert(write(fd, contents, size) == static_cast<ssize_t>(size));
  close(fd);
}

} // namespace

int main() {
  char root_template[] = "/tmp/oos-device-storage-XXXXXX";
  const char *root = mkdtemp(root_template);
  assert(root);
  const std::string internal = std::string(root) + "/internal";
  const std::string removable = std::string(root) + "/removable";
  const std::string games = internal + "/games";
  assert(mkdir(internal.c_str(), 0700) == 0);
  assert(mkdir(removable.c_str(), 0700) == 0);
  assert(mkdir(games.c_str(), 0700) == 0);
  writeFile(games + "/demo.jar", "jar-data");
  writeFile(removable + "/card.jar", "card-data");

  oos::storage::DeviceStorageService service(internal, removable);
  std::vector<oos::storage::DeviceStorageEntry> entries;
  assert(service.list(oos::storage::DeviceStorageVolume::Internal, entries));
  assert(entries.size() == 1);
  assert(entries[0].path == "games/demo.jar");
  assert(entries[0].size == 8);
  std::vector<uint8_t> bytes;
  assert(service.read(oos::storage::DeviceStorageVolume::Internal,
                      "games/demo.jar", bytes));
  assert(std::string(bytes.begin(), bytes.end()) == "jar-data");
  assert(!service.read(oos::storage::DeviceStorageVolume::Internal,
                       "../outside", bytes));

  int sockets[2] = {-1, -1};
  assert(oos_device_api_socket_pair(sockets) == 0);
  std::thread responder([&] {
    OosDeviceApiRequest request = {};
    assert(oos_device_api_receive(sockets[0], &request, 1000) == 1);
    assert(request.operation == OOS_DEVICE_API_LIST_FILES);
    assert(request.volume == OOS_DEVICE_API_INTERNAL);
    constexpr char response[] = "[]";
    assert(oos_device_api_reply(sockets[0], 0, response, 2, 1000) == 0);
  });
  void *payload = nullptr;
  uint32_t payload_size = 0;
  const int request_result = oos_device_api_request(
      sockets[1], OOS_DEVICE_API_LIST_FILES, OOS_DEVICE_API_INTERNAL, "",
      &payload, &payload_size, 1000);
  if (request_result != 0)
    std::fprintf(stderr, "device API request failed: %d\n", request_result);
  assert(request_result == 0);
  assert(payload_size == 2);
  assert(std::memcmp(payload, "[]", 2) == 0);
  oos_device_api_free(payload);
  responder.join();
  close(sockets[0]);
  close(sockets[1]);

  assert(unlink((games + "/demo.jar").c_str()) == 0);
  assert(unlink((removable + "/card.jar").c_str()) == 0);
  assert(rmdir(games.c_str()) == 0);
  assert(rmdir(internal.c_str()) == 0);
  assert(rmdir(removable.c_str()) == 0);
  assert(rmdir(root) == 0);
  return 0;
}
