#include "oos/storage/filesystem.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace oos::storage {
namespace {

bool writeAll(int fd, const uint8_t *data, size_t size, std::string &error) {
  while (size > 0) {
    const ssize_t count = write(fd, data, size);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      error = std::string("write: ") + std::strerror(errno);
      return false;
    }
    data += count;
    size -= static_cast<size_t>(count);
  }
  return true;
}

std::string temporaryPath(const std::string &path) {
  char pid[32] = {};
  std::snprintf(pid, sizeof(pid), "%lu", static_cast<unsigned long>(getpid()));
  return path + ".tmp." + pid;
}

} // namespace

std::string parentPath(const std::string &path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos)
    return ".";
  return slash == 0 ? "/" : path.substr(0, slash);
}

bool ensureDirectory(const std::string &path, uint32_t mode,
                     std::string &error) {
  if (path.empty()) {
    error = "directory path is empty";
    return false;
  }
  std::string current;
  size_t position = 0;
  if (path.front() == '/') {
    current = "/";
    position = 1;
  }
  while (position <= path.size()) {
    const size_t slash = path.find('/', position);
    const std::string component = path.substr(
        position,
        (slash == std::string::npos ? path.size() : slash) - position);
    if (!component.empty()) {
      if (component == "." || component == "..") {
        error = "directory path contains traversal components";
        return false;
      }
      if (!current.empty() && current.back() != '/')
        current.push_back('/');
      current += component;
      if (mkdir(current.c_str(), static_cast<mode_t>(mode)) != 0 &&
          errno != EEXIST) {
        error = "mkdir " + current + ": " + std::strerror(errno);
        return false;
      }
      struct stat status = {};
      if (stat(current.c_str(), &status) != 0 || !S_ISDIR(status.st_mode)) {
        error = current + " is not a directory";
        return false;
      }
    }
    if (slash == std::string::npos)
      break;
    position = slash + 1;
  }
  return true;
}

bool readFile(const std::string &path, std::vector<uint8_t> &output,
              size_t maximum_bytes, std::string &error) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    error = "open " + path + ": " + std::strerror(errno);
    return false;
  }
  struct stat status = {};
  if (fstat(fd, &status) != 0 || status.st_size < 0 ||
      static_cast<uint64_t>(status.st_size) > maximum_bytes) {
    error = "file is invalid or exceeds size limit: " + path;
    close(fd);
    return false;
  }
  output.assign(static_cast<size_t>(status.st_size), 0);
  size_t offset = 0;
  while (offset < output.size()) {
    const ssize_t count =
        read(fd, output.data() + offset, output.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      error = "read " + path + ": " + std::strerror(errno);
      close(fd);
      return false;
    }
    offset += static_cast<size_t>(count);
  }
  close(fd);
  return true;
}

bool writeFileAtomic(const std::string &path, const uint8_t *data, size_t size,
                     uint32_t mode, std::string &error) {
  if (!ensureDirectory(parentPath(path), 0700, error))
    return false;
  const std::string temporary = temporaryPath(path);
  const int fd =
      open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
           static_cast<mode_t>(mode));
  if (fd < 0) {
    error = "create " + temporary + ": " + std::strerror(errno);
    return false;
  }
  bool success = writeAll(fd, data, size, error);
  if (success && fsync(fd) != 0) {
    error = "fsync " + temporary + ": " + std::strerror(errno);
    success = false;
  }
  if (close(fd) != 0 && success) {
    error = "close " + temporary + ": " + std::strerror(errno);
    success = false;
  }
  if (success && rename(temporary.c_str(), path.c_str()) != 0) {
    error = "rename " + temporary + ": " + std::strerror(errno);
    success = false;
  }
  if (!success)
    unlink(temporary.c_str());
  return success;
}

bool copyFileAtomic(const std::string &source, const std::string &destination,
                    uint32_t mode, std::string &error) {
  std::vector<uint8_t> bytes;
  if (!readFile(source, bytes, 256 * 1024 * 1024, error))
    return false;
  return writeFileAtomic(destination, bytes.data(), bytes.size(), mode, error);
}

} // namespace oos::storage
