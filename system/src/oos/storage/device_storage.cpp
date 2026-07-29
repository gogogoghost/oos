#include "oos/storage/device_storage.h"

#include "oos/apps/zip_archive.h"
#include "oos/storage/filesystem.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace oos::storage {
namespace {

constexpr unsigned kMaximumDepth = 16;
constexpr size_t kMaximumEntries = 8192;
constexpr size_t kMaximumReadBytes = 64 * 1024 * 1024;

void collectFiles(const std::string &root, const std::string &relative,
                  unsigned depth, std::vector<DeviceStorageEntry> &entries) {
  if (depth > kMaximumDepth || entries.size() >= kMaximumEntries)
    return;
  const std::string directory_path =
      relative.empty() ? root : root + "/" + relative;
  DIR *directory = opendir(directory_path.c_str());
  if (!directory)
    return;
  while (dirent *entry = readdir(directory)) {
    if (std::strcmp(entry->d_name, ".") == 0 ||
        std::strcmp(entry->d_name, "..") == 0)
      continue;
    const std::string name =
        relative.empty() ? entry->d_name : relative + "/" + entry->d_name;
    const std::string path = root + "/" + name;
    struct stat status = {};
    if (lstat(path.c_str(), &status) != 0 || S_ISLNK(status.st_mode))
      continue;
    if (S_ISDIR(status.st_mode)) {
      collectFiles(root, name, depth + 1, entries);
    } else if (S_ISREG(status.st_mode)) {
      entries.push_back(
          DeviceStorageEntry{name, static_cast<uint64_t>(status.st_size),
                             static_cast<int64_t>(status.st_mtime) * 1000});
    }
    if (entries.size() >= kMaximumEntries)
      break;
  }
  closedir(directory);
}

} // namespace

DeviceStorageService::DeviceStorageService(std::string internal_root,
                                           std::string removable_root)
    : internal_root_(std::move(internal_root)),
      removable_root_(std::move(removable_root)) {}

const std::string &
DeviceStorageService::root(DeviceStorageVolume volume) const {
  return volume == DeviceStorageVolume::Internal ? internal_root_
                                                 : removable_root_;
}

bool DeviceStorageService::list(DeviceStorageVolume volume,
                                std::vector<DeviceStorageEntry> &entries) {
  error_.clear();
  entries.clear();
  const std::string &storage_root = root(volume);
  struct stat status = {};
  if (stat(storage_root.c_str(), &status) != 0 || !S_ISDIR(status.st_mode)) {
    error_ = "device storage is unavailable: " + storage_root;
    return false;
  }
  collectFiles(storage_root, "", 0, entries);
  std::sort(
      entries.begin(), entries.end(),
      [](const DeviceStorageEntry &left, const DeviceStorageEntry &right) {
        return left.path < right.path;
      });
  return true;
}

bool DeviceStorageService::read(DeviceStorageVolume volume,
                                const std::string &path,
                                std::vector<uint8_t> &bytes) {
  error_.clear();
  if (!apps::validPackagePath(path) || path.back() == '/') {
    error_ = "device storage path is invalid";
    return false;
  }
  return readFile(root(volume) + "/" + path, bytes, kMaximumReadBytes, error_);
}

} // namespace oos::storage
