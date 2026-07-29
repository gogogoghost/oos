#include "oos/storage/device_storage.h"

#include "oos/apps/zip_archive.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace oos::storage {
namespace {

constexpr unsigned kMaximumDepth = 16;
constexpr size_t kMaximumEntries = 8192;
constexpr size_t kMaximumReadBytes = 64 * 1024 * 1024;
std::atomic<uint32_t> gTemporarySequence{0};

bool validFilePath(const std::string &path) {
  return apps::validPackagePath(path) && path.back() != '/';
}

bool writeAll(int fd, const uint8_t *bytes, size_t size, std::string &error) {
  while (size) {
    const ssize_t written = ::write(fd, bytes, size);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      error = std::string("write device storage: ") + std::strerror(errno);
      return false;
    }
    bytes += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

bool openParentDirectory(const std::string &root, const std::string &path,
                         bool create, int &parent, std::string &name,
                         std::string &error) {
  parent = open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (parent < 0) {
    error = "open device storage root " + root + ": " + std::strerror(errno);
    return false;
  }
  size_t start = 0;
  while (true) {
    const size_t slash = path.find('/', start);
    if (slash == std::string::npos) {
      name = path.substr(start);
      return true;
    }
    const std::string component = path.substr(start, slash - start);
    if (create && mkdirat(parent, component.c_str(), 0700) != 0 &&
        errno != EEXIST) {
      error = "create device storage directory " + component + ": " +
              std::strerror(errno);
      close(parent);
      parent = -1;
      return false;
    }
    const int next = openat(parent, component.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (next < 0) {
      error = "open device storage directory " + component + ": " +
              std::strerror(errno);
      close(parent);
      parent = -1;
      return false;
    }
    close(parent);
    parent = next;
    start = slash + 1;
  }
}

int openStorageFile(const std::string &root, const std::string &path, int flags,
                    mode_t mode, bool create_directories, std::string &error) {
  int parent = -1;
  std::string name;
  if (!openParentDirectory(root, path, create_directories, parent, name, error))
    return -1;
  const int fd =
      openat(parent, name.c_str(), flags | O_CLOEXEC | O_NOFOLLOW, mode);
  if (fd < 0)
    error = "open device storage file " + path + ": " + std::strerror(errno);
  close(parent);
  return fd;
}

bool writeAtomicAt(int parent, const std::string &name, const uint8_t *bytes,
                   size_t size, std::string &error) {
  char temporary[64] = {};
  int fd = -1;
  for (unsigned attempt = 0; attempt < 32 && fd < 0; ++attempt) {
    const uint32_t sequence = gTemporarySequence.fetch_add(1);
    std::snprintf(temporary, sizeof(temporary), ".oos-tmp-%lu-%u",
                  static_cast<unsigned long>(getpid()), sequence);
    fd = openat(parent, temporary,
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 && errno != EEXIST)
      break;
  }
  if (fd < 0) {
    error = "create device storage temporary file: " +
            std::string(std::strerror(errno));
    return false;
  }
  bool success = writeAll(fd, bytes, size, error);
  if (success && fsync(fd) != 0) {
    error = "fsync device storage temporary file: " +
            std::string(std::strerror(errno));
    success = false;
  }
  if (close(fd) != 0 && success) {
    error = "close device storage temporary file: " +
            std::string(std::strerror(errno));
    success = false;
  }
  if (success && renameat(parent, temporary, parent, name.c_str()) != 0) {
    error = "replace device storage file: " + std::string(std::strerror(errno));
    success = false;
  }
  if (!success)
    unlinkat(parent, temporary, 0);
  return success;
}

void collectFiles(int directory_fd, const std::string &relative, unsigned depth,
                  std::vector<DeviceStorageEntry> &entries) {
  if (depth > kMaximumDepth || entries.size() >= kMaximumEntries)
    return;
  const int scan_fd = dup(directory_fd);
  DIR *directory = scan_fd < 0 ? nullptr : fdopendir(scan_fd);
  if (!directory) {
    if (scan_fd >= 0)
      close(scan_fd);
    return;
  }
  while (dirent *entry = readdir(directory)) {
    if (std::strcmp(entry->d_name, ".") == 0 ||
        std::strcmp(entry->d_name, "..") == 0)
      continue;
    const std::string name =
        relative.empty() ? entry->d_name : relative + "/" + entry->d_name;
    struct stat status = {};
    if (fstatat(directory_fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) !=
            0 ||
        S_ISLNK(status.st_mode))
      continue;
    if (S_ISDIR(status.st_mode)) {
      const int child = openat(directory_fd, entry->d_name,
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (child >= 0) {
        collectFiles(child, name, depth + 1, entries);
        close(child);
      }
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
  const int root_fd = open(storage_root.c_str(),
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (root_fd < 0 || fstat(root_fd, &status) != 0 || !S_ISDIR(status.st_mode)) {
    if (root_fd >= 0)
      close(root_fd);
    error_ = "device storage is unavailable: " + storage_root;
    return false;
  }
  collectFiles(root_fd, "", 0, entries);
  close(root_fd);
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
  uint64_t size = 0;
  if (!fileSize(volume, path, size))
    return false;
  bytes.assign(static_cast<size_t>(size), 0);
  size_t bytes_read = 0;
  if (!readInto(volume, path, bytes.data(), bytes.size(), bytes_read)) {
    bytes.clear();
    return false;
  }
  if (bytes_read != bytes.size()) {
    error_ = "device storage file changed while reading";
    bytes.clear();
    return false;
  }
  return true;
}

bool DeviceStorageService::fileSize(DeviceStorageVolume volume,
                                    const std::string &path, uint64_t &size) {
  error_.clear();
  size = 0;
  if (!validFilePath(path)) {
    error_ = "device storage path is invalid";
    return false;
  }
  const int fd =
      openStorageFile(root(volume), path, O_RDONLY, 0, false, error_);
  if (fd < 0) {
    return false;
  }
  struct stat status = {};
  const bool valid = fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
                     status.st_size >= 0 &&
                     static_cast<uint64_t>(status.st_size) <= kMaximumReadBytes;
  close(fd);
  if (!valid) {
    error_ = "device storage file is invalid or exceeds size limit";
    return false;
  }
  size = static_cast<uint64_t>(status.st_size);
  return true;
}

bool DeviceStorageService::readInto(DeviceStorageVolume volume,
                                    const std::string &path, uint8_t *bytes,
                                    size_t capacity, size_t &bytes_read) {
  error_.clear();
  bytes_read = 0;
  if (!validFilePath(path) || (capacity && !bytes) ||
      capacity > kMaximumReadBytes) {
    error_ = "device storage read arguments are invalid";
    return false;
  }
  const int fd =
      openStorageFile(root(volume), path, O_RDONLY, 0, false, error_);
  if (fd < 0) {
    return false;
  }
  struct stat status = {};
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0 || static_cast<uint64_t>(status.st_size) > capacity) {
    error_ = "device storage file changed or exceeds read buffer";
    close(fd);
    return false;
  }
  const size_t expected = static_cast<size_t>(status.st_size);
  while (bytes_read < expected) {
    const ssize_t count = ::read(fd, bytes + bytes_read, expected - bytes_read);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      error_ = "read device storage file " + path + ": " + std::strerror(errno);
      close(fd);
      return false;
    }
    bytes_read += static_cast<size_t>(count);
  }
  close(fd);
  return true;
}

bool DeviceStorageService::write(DeviceStorageVolume volume,
                                 const std::string &path,
                                 DeviceStorageWriteMode mode,
                                 const uint8_t *bytes, size_t size) {
  error_.clear();
  if (!validFilePath(path) || (size && !bytes) || size > kMaximumReadBytes) {
    error_ = "device storage write arguments are invalid";
    return false;
  }
  if (mode != DeviceStorageWriteMode::Create &&
      mode != DeviceStorageWriteMode::Replace &&
      mode != DeviceStorageWriteMode::Append) {
    error_ = "device storage write mode is invalid";
    return false;
  }
  int parent = -1;
  std::string name;
  if (!openParentDirectory(root(volume), path, true, parent, name, error_))
    return false;
  if (mode == DeviceStorageWriteMode::Replace) {
    const bool success = writeAtomicAt(parent, name, bytes, size, error_);
    close(parent);
    return success;
  }
  const int flags =
      O_WRONLY | O_CREAT | O_CLOEXEC |
      (mode == DeviceStorageWriteMode::Create ? O_EXCL : O_APPEND);
  const int fd = openat(parent, name.c_str(), flags | O_NOFOLLOW, 0600);
  if (fd < 0) {
    error_ = "open device storage file " + path + ": " + std::strerror(errno);
    close(parent);
    return false;
  }
  bool success = writeAll(fd, bytes, size, error_);
  if (success && fsync(fd) != 0) {
    error_ = "fsync device storage file " + path + ": " + std::strerror(errno);
    success = false;
  }
  if (close(fd) != 0 && success) {
    error_ = "close device storage file " + path + ": " + std::strerror(errno);
    success = false;
  }
  if (!success && mode == DeviceStorageWriteMode::Create)
    unlinkat(parent, name.c_str(), 0);
  close(parent);
  return success;
}

bool DeviceStorageService::remove(DeviceStorageVolume volume,
                                  const std::string &path, bool &removed) {
  error_.clear();
  removed = false;
  if (!validFilePath(path)) {
    error_ = "device storage path is invalid";
    return false;
  }
  int parent = -1;
  std::string name;
  if (!openParentDirectory(root(volume), path, false, parent, name, error_)) {
    const int open_error = errno;
    if (open_error == ENOENT) {
      error_.clear();
      return true;
    }
    return false;
  }
  struct stat status = {};
  if (fstatat(parent, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) {
      close(parent);
      return true;
    }
    error_ = "stat device storage file " + path + ": " + std::strerror(errno);
    close(parent);
    return false;
  }
  if (!S_ISREG(status.st_mode)) {
    error_ = "device storage path is not a regular file";
    close(parent);
    return false;
  }
  if (unlinkat(parent, name.c_str(), 0) != 0) {
    error_ = "delete device storage file " + path + ": " + std::strerror(errno);
    close(parent);
    return false;
  }
  close(parent);
  removed = true;
  return true;
}

bool DeviceStorageService::space(DeviceStorageVolume volume, bool free,
                                 uint64_t &bytes) {
  error_.clear();
  bytes = 0;
  const std::string &storage_root = root(volume);
  const int root_fd = open(storage_root.c_str(),
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  struct statvfs status = {};
  if (root_fd < 0 || fstatvfs(root_fd, &status) != 0) {
    if (root_fd >= 0)
      close(root_fd);
    error_ = "statvfs " + storage_root + ": " + std::strerror(errno);
    return false;
  }
  close(root_fd);
  const uint64_t blocks =
      free ? status.f_bavail : status.f_blocks - status.f_bfree;
  const uint64_t block_size =
      status.f_frsize ? status.f_frsize : status.f_bsize;
  if (!block_size) {
    error_ = "device storage filesystem reports a zero block size";
    return false;
  }
  bytes = blocks > UINT64_MAX / block_size ? UINT64_MAX : blocks * block_size;
  return true;
}

bool DeviceStorageService::freeSpace(DeviceStorageVolume volume,
                                     uint64_t &bytes) {
  return space(volume, true, bytes);
}

bool DeviceStorageService::usedSpace(DeviceStorageVolume volume,
                                     uint64_t &bytes) {
  return space(volume, false, bytes);
}

} // namespace oos::storage
