#pragma once

#include <cstdint>
#include <memory>

struct WPEAndroidBuffer;
struct WPEAndroidViewBackend;

namespace oos::nokia2780 {

class WpeDisplayManager {
public:
  static constexpr uint32_t kPrimaryWidth = 240;
  static constexpr uint32_t kPrimaryHeight = 320;

  explicit WpeDisplayManager(bool reveal_first_frame = true);
  ~WpeDisplayManager();

  WpeDisplayManager(const WpeDisplayManager &) = delete;
  WpeDisplayManager &operator=(const WpeDisplayManager &) = delete;

  bool initialize();
  void present(WPEAndroidViewBackend *backend, WPEAndroidBuffer *buffer,
               int acquire_fence_fd);
  void refresh();

  bool frameReady();
  bool showCover(const uint16_t *rgb565_pixels);
  bool showPrimary();
  void shutdownDisplays();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::nokia2780
