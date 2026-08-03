#include "oos/resources/package_assets.h"

#include "oos/apps/zip_archive.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace oos::resources {

PackageAssetService::PackageAssetService(std::string root)
    : root_(std::move(root)) {}

PackageAssetService::~PackageAssetService() { closeAll(); }

PackageAssetService::Entry *PackageAssetService::find(uint32_t handle) {
  for (Entry &entry : entries_) {
    if (entry.handle == handle)
      return &entry;
  }
  return nullptr;
}

bool PackageAssetService::open(const std::string &path, uint32_t &handle,
                               uint64_t &size) {
  error_.clear();
  handle = 0;
  size = 0;
  if (entries_.size() >= kMaximumOpenAssets || path.empty() ||
      path.back() == '/' || !apps::validPackagePath(path)) {
    error_ = "invalid asset path or open-asset limit reached";
    return false;
  }
  const int root =
      ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (root < 0) {
    error_ = "open asset root: " + std::string(std::strerror(errno));
    return false;
  }
  const int fd = openat(root, path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  const int saved_errno = errno;
  ::close(root);
  if (fd < 0) {
    error_ = "open packaged asset: " + std::string(std::strerror(saved_errno));
    return false;
  }
  struct stat status = {};
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0) {
    error_ = "packaged asset is not a regular file";
    ::close(fd);
    return false;
  }
  if (next_handle_ == 0)
    next_handle_ = 1;
  handle = next_handle_++;
  size = static_cast<uint64_t>(status.st_size);
  entries_.push_back({handle, fd, size});
  return true;
}

bool PackageAssetService::readSize(uint32_t handle, uint64_t offset,
                                   uint32_t maximum_bytes, uint32_t &size) {
  error_.clear();
  size = 0;
  Entry *entry = find(handle);
  if (!entry || maximum_bytes > kMaximumReadBytes || offset > entry->size) {
    error_ = "invalid packaged asset read";
    return false;
  }
  const uint64_t remaining = entry->size - offset;
  size = static_cast<uint32_t>(
      std::min<uint64_t>(remaining, static_cast<uint64_t>(maximum_bytes)));
  return true;
}

bool PackageAssetService::readInto(uint32_t handle, uint64_t offset,
                                   uint8_t *output, uint32_t capacity,
                                   uint32_t &bytes_read) {
  error_.clear();
  bytes_read = 0;
  Entry *entry = find(handle);
  uint32_t expected = 0;
  if (!entry || !readSize(handle, offset, capacity, expected) ||
      expected != capacity || (capacity && !output)) {
    error_ = "invalid packaged asset destination";
    return false;
  }
  while (bytes_read < capacity) {
    const ssize_t result = pread(entry->fd, output + bytes_read,
                                 capacity - bytes_read, offset + bytes_read);
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0) {
      error_ = result == 0 ? "read packaged asset: unexpected end of file"
                           : "read packaged asset: " +
                                 std::string(std::strerror(errno));
      return false;
    }
    bytes_read += static_cast<uint32_t>(result);
  }
  return true;
}

bool PackageAssetService::read(uint32_t handle, uint64_t offset,
                               uint32_t maximum_bytes,
                               std::vector<uint8_t> &output) {
  output.clear();
  uint32_t size = 0;
  if (!readSize(handle, offset, maximum_bytes, size))
    return false;
  output.resize(size);
  uint32_t bytes_read = 0;
  if (readInto(handle, offset, output.data(), size, bytes_read))
    return true;
  output.clear();
  return false;
}

bool PackageAssetService::close(uint32_t handle) {
  error_.clear();
  for (auto entry = entries_.begin(); entry != entries_.end(); ++entry) {
    if (entry->handle != handle)
      continue;
    ::close(entry->fd);
    entries_.erase(entry);
    return true;
  }
  error_ = "unknown packaged asset handle";
  return false;
}

void PackageAssetService::closeAll() {
  for (const Entry &entry : entries_)
    ::close(entry.fd);
  entries_.clear();
}

} // namespace oos::resources
