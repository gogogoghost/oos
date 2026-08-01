#include "oos/storage/device_storage.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
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
  const std::string outside = std::string(root) + "/outside";
  assert(mkdir(outside.c_str(), 0700) == 0);
  writeFile(outside + "/secret.txt", "secret");
  assert(symlink(outside.c_str(), (internal + "/linked").c_str()) == 0);

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
  assert(!service.read(oos::storage::DeviceStorageVolume::Internal,
                       "linked/secret.txt", bytes));
  constexpr uint8_t first[] = {'f', 'i', 'r', 's', 't'};
  assert(!service.write(
      oos::storage::DeviceStorageVolume::Internal, "linked/created.txt",
      oos::storage::DeviceStorageWriteMode::Create, first, sizeof(first)));
  assert(service.write(
      oos::storage::DeviceStorageVolume::Internal, "documents/note.txt",
      oos::storage::DeviceStorageWriteMode::Create, first, sizeof(first)));
  assert(!service.write(
      oos::storage::DeviceStorageVolume::Internal, "documents/note.txt",
      oos::storage::DeviceStorageWriteMode::Create, first, sizeof(first)));
  constexpr uint8_t suffix[] = {'-', 'a', 'p', 'p', 'e', 'n', 'd'};
  assert(service.write(
      oos::storage::DeviceStorageVolume::Internal, "documents/note.txt",
      oos::storage::DeviceStorageWriteMode::Append, suffix, sizeof(suffix)));
  assert(service.read(oos::storage::DeviceStorageVolume::Internal,
                      "documents/note.txt", bytes));
  assert(std::string(bytes.begin(), bytes.end()) == "first-append");
  constexpr uint8_t replacement[] = {'r', 'e', 'p', 'l', 'a', 'c', 'e', 'd'};
  assert(service.write(oos::storage::DeviceStorageVolume::Internal,
                       "documents/note.txt",
                       oos::storage::DeviceStorageWriteMode::Replace,
                       replacement, sizeof(replacement)));
  uint64_t free_bytes = 0;
  uint64_t used_bytes = 0;
  assert(service.freeSpace(oos::storage::DeviceStorageVolume::Internal,
                           free_bytes));
  assert(service.usedSpace(oos::storage::DeviceStorageVolume::Internal,
                           used_bytes));
  assert(free_bytes > 0 && used_bytes > 0);

  bool removed = false;
  assert(service.remove(oos::storage::DeviceStorageVolume::Internal,
                        "documents/note.txt", removed));
  assert(removed);
  assert(service.remove(oos::storage::DeviceStorageVolume::Internal,
                        "documents/note.txt", removed));
  assert(!removed);
  assert(unlink((games + "/demo.jar").c_str()) == 0);
  assert(unlink((removable + "/card.jar").c_str()) == 0);
  assert(unlink((internal + "/linked").c_str()) == 0);
  assert(unlink((outside + "/secret.txt").c_str()) == 0);
  assert(rmdir(games.c_str()) == 0);
  assert(rmdir((internal + "/documents").c_str()) == 0);
  assert(rmdir(internal.c_str()) == 0);
  assert(rmdir(removable.c_str()) == 0);
  assert(rmdir(outside.c_str()) == 0);
  assert(rmdir(root) == 0);
  return 0;
}
