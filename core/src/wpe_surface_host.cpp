#include "oos/web/wpe_surface_host.h"

#include "oos/compositor/surface.h"

#include <wpe-android/view-backend.h>

#include <cstdio>

namespace oos::web {

class WpeSurfaceHost::Impl {
public:
  Impl(compositor::SurfaceSink &surface_sink, uint64_t surface_id,
       uint32_t width, uint32_t height)
      : surface_sink_(surface_sink), surface_id_(surface_id), width_(width),
        height_(height) {}

  ~Impl() {
    if (backend_)
      WPEAndroidViewBackend_destroy(backend_);
  }

  bool initialize() {
    if (backend_)
      return true;
    backend_ = WPEAndroidViewBackend_create(width_, height_);
    if (!backend_)
      return false;
    WPEAndroidViewBackend_setCommitBufferHandler(backend_, this, commitBuffer);
    return true;
  }

  wpe_view_backend *viewBackend() const {
    return backend_ ? WPEAndroidViewBackend_getWPEViewBackend(backend_)
                    : nullptr;
  }

  uint64_t presentedFrames() const { return presented_frames_; }

private:
  static void commitBuffer(void *data, WPEAndroidBuffer *buffer,
                           int acquire_fence_fd) {
    static_cast<Impl *>(data)->commit(buffer, acquire_fence_fd);
  }

  void commit(WPEAndroidBuffer *buffer, int acquire_fence_fd) {
    compositor::SurfaceFrame frame;
    frame.surface_id = surface_id_;
    frame.buffer = WPEAndroidBuffer_getAHardwareBuffer(buffer);
    frame.buffer_width = width_;
    frame.buffer_height = height_;
    frame.acquire_fence_fd = acquire_fence_fd;
    frame.width = width_;
    frame.height = height_;
    const bool presented = surface_sink_.presentSurface(frame);
    WPEAndroidViewBackend_dispatchReleaseBuffer(backend_, buffer);
    if (!presented) {
      std::fprintf(stderr, "OOS compositor rejected WPE surface %llu frame\n",
                   static_cast<unsigned long long>(surface_id_));
      return;
    }
    ++presented_frames_;
    WPEAndroidViewBackend_dispatchFrameComplete(backend_);
  }

  compositor::SurfaceSink &surface_sink_;
  const uint64_t surface_id_;
  const uint32_t width_;
  const uint32_t height_;
  WPEAndroidViewBackend *backend_ = nullptr;
  uint64_t presented_frames_ = 0;
};

WpeSurfaceHost::WpeSurfaceHost(compositor::SurfaceSink &surface_sink,
                               uint64_t surface_id, uint32_t width,
                               uint32_t height)
    : impl_(std::make_unique<Impl>(surface_sink, surface_id, width, height)) {}

WpeSurfaceHost::~WpeSurfaceHost() = default;

bool WpeSurfaceHost::initialize() { return impl_->initialize(); }

wpe_view_backend *WpeSurfaceHost::viewBackend() const {
  return impl_->viewBackend();
}

uint64_t WpeSurfaceHost::presentedFrames() const {
  return impl_->presentedFrames();
}

} // namespace oos::web
