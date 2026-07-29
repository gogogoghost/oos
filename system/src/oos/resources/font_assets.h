#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace oos::resources {

enum class FontRole : uint32_t {
  UiProportional = 0,
  UiMonospace = 1,
  Emoji = 2,
};

enum class FontAssetStatus {
  Ok,
  Unavailable,
  InvalidArgument,
  LimitExceeded,
  Io,
};

// Read-only system font provider shared by native application backends. Font
// roles map to fixed files, so applications cannot traverse the host rootfs.
class FontAssetService {
public:
  static constexpr uint64_t kMaximumFontBytes = 4 * 1024 * 1024;

  explicit FontAssetService(std::string root = "/opt/oos/share/fonts");

  FontAssetStatus fileSize(FontRole role, uint64_t &size);
  FontAssetStatus readInto(FontRole role, uint8_t *bytes, size_t capacity,
                           size_t &bytes_read);

  const std::string &lastError() const { return error_; }

private:
  FontAssetStatus openFont(FontRole role, int &fd, uint64_t &size);

  std::string root_;
  std::string error_;
};

} // namespace oos::resources
