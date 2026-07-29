#include <cassert>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "oos/resources/font_assets.h"

namespace {

bool writeAll(int fd, const char *bytes, size_t size) {
  while (size) {
    const ssize_t written = write(fd, bytes, size);
    if (written <= 0)
      return false;
    bytes += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

} // namespace

int main() {
  char root_template[] = "/tmp/oos-font-assets.XXXXXX";
  const char *root = mkdtemp(root_template);
  assert(root);

  oos::resources::FontAssetService fonts(root);
  uint64_t size = 0;
  assert(fonts.fileSize(oos::resources::FontRole::UiProportional, size) ==
         oos::resources::FontAssetStatus::Unavailable);
  assert(fonts.fileSize(static_cast<oos::resources::FontRole>(99), size) ==
         oos::resources::FontAssetStatus::InvalidArgument);
  assert(fonts.fileSize(oos::resources::FontRole::UiMonospace, size) ==
         oos::resources::FontAssetStatus::Unavailable);

  const std::string path = std::string(root) + "/ui-proportional.otf";
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  assert(fd >= 0);
  constexpr char kFontMarker[] = "OTTOtest-font";
  assert(writeAll(fd, kFontMarker, sizeof(kFontMarker) - 1));
  assert(close(fd) == 0);

  assert(fonts.fileSize(oos::resources::FontRole::UiProportional, size) ==
         oos::resources::FontAssetStatus::Ok);
  assert(size == sizeof(kFontMarker) - 1);
  char result[sizeof(kFontMarker) - 1] = {};
  size_t bytes_read = 0;
  assert(fonts.readInto(oos::resources::FontRole::UiProportional,
                        reinterpret_cast<uint8_t *>(result), sizeof(result),
                        bytes_read) == oos::resources::FontAssetStatus::Ok);
  assert(bytes_read == sizeof(result));
  assert(std::string(result, sizeof(result)) ==
         std::string(kFontMarker, sizeof(kFontMarker) - 1));

  assert(unlink(path.c_str()) == 0);
  assert(symlink("/etc/passwd", path.c_str()) == 0);
  assert(fonts.fileSize(oos::resources::FontRole::UiProportional, size) ==
         oos::resources::FontAssetStatus::Unavailable);
  assert(unlink(path.c_str()) == 0);

  fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  assert(fd >= 0);
  assert(ftruncate(fd, oos::resources::FontAssetService::kMaximumFontBytes +
                           1) == 0);
  assert(close(fd) == 0);
  assert(fonts.fileSize(oos::resources::FontRole::UiProportional, size) ==
         oos::resources::FontAssetStatus::LimitExceeded);

  std::filesystem::remove_all(root);
  std::puts("Font asset role, bounds, and symlink checks passed");
  return 0;
}
