#include "oos/resources/font_assets.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <utility>

namespace oos::resources {
namespace {

constexpr char kDefaultFontRoot[] = "/opt/oos/share/fonts";

const char *fontFileName(FontRole role) {
  switch (role) {
  case FontRole::UiProportional:
    return "ui-proportional.otf";
  case FontRole::UiMonospace:
  case FontRole::Emoji:
    return nullptr;
  case FontRole::CjkFallback:
    return "cjk-fallback.ttf";
  }
  return nullptr;
}

bool validRole(FontRole role) {
  return static_cast<uint32_t>(role) <=
         static_cast<uint32_t>(FontRole::CjkFallback);
}

int openSystemUiFont() {
  constexpr const char *candidates[] = {
      "/system/fonts/Roboto-Regular.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"};
  for (const char *candidate : candidates) {
    const int fd = open(candidate, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0)
      return fd;
  }
  return -1;
}

int openSystemCjkFont() {
  constexpr const char *candidates[] = {
      "/system/fonts/DroidSansFallback.ttf",
      "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"};
  for (const char *candidate : candidates) {
    const int fd = open(candidate, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0)
      return fd;
  }
  return -1;
}

} // namespace

FontAssetService::FontAssetService(std::string root) : root_(std::move(root)) {}

FontAssetStatus FontAssetService::openFont(FontRole role, int &fd,
                                           uint64_t &size) {
  error_.clear();
  fd = -1;
  size = 0;
  if (!validRole(role)) {
    error_ = "invalid font role";
    return FontAssetStatus::InvalidArgument;
  }
  const char *name = fontFileName(role);
  if (!name) {
    error_ = "font role is not installed";
    return FontAssetStatus::Unavailable;
  }
  const int root =
      open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (root >= 0) {
    fd = openat(root, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    close(root);
  }
  if (fd < 0 && role == FontRole::UiProportional && root_ == kDefaultFontRoot) {
    fd = openSystemUiFont();
  }
  if (fd < 0 && role == FontRole::CjkFallback && root_ == kDefaultFontRoot)
    fd = openSystemCjkFont();
  if (fd < 0) {
    error_ = std::string("font asset is unavailable: ") + name;
    return FontAssetStatus::Unavailable;
  }
  struct stat status = {};
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0) {
    error_ = std::string("invalid font asset: ") + name;
    close(fd);
    fd = -1;
    return FontAssetStatus::Io;
  }
  size = static_cast<uint64_t>(status.st_size);
  if (size == 0) {
    error_ = std::string("empty font asset: ") + name;
    close(fd);
    fd = -1;
    return FontAssetStatus::Io;
  }
  if (size > kMaximumFontBytes) {
    error_ = std::string("font asset exceeds size limit: ") + name;
    close(fd);
    fd = -1;
    return FontAssetStatus::LimitExceeded;
  }
  return FontAssetStatus::Ok;
}

FontAssetStatus FontAssetService::fileSize(FontRole role, uint64_t &size) {
  int fd = -1;
  const FontAssetStatus result = openFont(role, fd, size);
  if (fd >= 0)
    close(fd);
  return result;
}

FontAssetStatus FontAssetService::readInto(FontRole role, uint8_t *bytes,
                                           size_t capacity,
                                           size_t &bytes_read) {
  bytes_read = 0;
  int fd = -1;
  uint64_t native_size = 0;
  const FontAssetStatus opened = openFont(role, fd, native_size);
  if (opened != FontAssetStatus::Ok)
    return opened;
  if (native_size != capacity || (capacity != 0 && !bytes)) {
    error_ = "font asset changed while reading";
    close(fd);
    return FontAssetStatus::Io;
  }
  while (bytes_read < capacity) {
    const ssize_t read_size =
        read(fd, bytes + bytes_read, capacity - bytes_read);
    if (read_size < 0 && errno == EINTR)
      continue;
    if (read_size <= 0) {
      error_ =
          std::string("read font asset: ") +
          (read_size == 0 ? "unexpected end of file" : std::strerror(errno));
      close(fd);
      return FontAssetStatus::Io;
    }
    bytes_read += static_cast<size_t>(read_size);
  }
  close(fd);
  return FontAssetStatus::Ok;
}

} // namespace oos::resources
