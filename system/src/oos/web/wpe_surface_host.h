#pragma once

#include <cstdint>
#include <memory>

struct wpe_view_backend;

namespace oos::compositor {
class SurfaceSink;
}

namespace oos::web {

// Adapts WPE's produced buffers to the OOS host compositor. WPE never owns or
// accesses a physical display through this interface.
class WpeSurfaceHost {
public:
  WpeSurfaceHost(compositor::SurfaceSink &surface_sink, uint64_t surface_id,
                 uint32_t width, uint32_t height);
  ~WpeSurfaceHost();

  WpeSurfaceHost(const WpeSurfaceHost &) = delete;
  WpeSurfaceHost &operator=(const WpeSurfaceHost &) = delete;

  bool initialize();
  wpe_view_backend *viewBackend() const;
  uint64_t presentedFrames() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::web
