#include "oos/web/wpe_surface_host.h"

#include "oos/compositor/surface.h"

#include <wpe-android/view-backend.h>
#include <wpe/wpe.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace oos::web {

class WpeSurfaceHost::Impl {
public:
  Impl(compositor::SurfaceSink &surface_sink, uint64_t surface_id,
       uint32_t width, uint32_t height)
      : surface_sink_(surface_sink), surface_id_(surface_id), width_(width),
        height_(height) {
    const char *trace = std::getenv("OOS_TRACE_WPE_FRAMES");
    trace_frames_ = trace && trace[0] && std::strcmp(trace, "0") != 0;
  }

  ~Impl() {
    if (trace_frames_) {
      const size_t pending = static_cast<size_t>(std::count_if(
          pending_buffers_.begin(), pending_buffers_.end(),
          [](const PendingBuffer &buffer) { return buffer.sequence != 0; }));
      std::fprintf(stderr, "WPE surface shutdown: pending=%zu\n", pending);
    }
    surface_sink_.cancelSubmissions(this);
    if (backend_) {
      for (PendingBuffer &pending : pending_buffers_) {
        if (pending.sequence)
          WPEAndroidViewBackend_dispatchReleaseBuffer(backend_, pending.buffer);
        pending = {};
      }
      if (trace_frames_)
        std::fprintf(stderr, "WPE surface shutdown: destroy backend\n");
      WPEAndroidViewBackend_destroy(backend_);
    }
    if (trace_frames_)
      std::fprintf(stderr, "WPE surface shutdown: complete\n");
  }

  bool initialize() {
    if (backend_)
      return true;
    backend_ = WPEAndroidViewBackend_create(width_, height_);
    if (!backend_)
      return false;
    wpe_view_backend_add_activity_state(viewBackend(),
                                        wpe_view_activity_state_visible |
                                            wpe_view_activity_state_focused |
                                            wpe_view_activity_state_in_window);
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

  static void submissionComplete(void *data, uint64_t sequence,
                                 bool presented) {
    static_cast<Impl *>(data)->complete(sequence, presented);
  }

  void commit(WPEAndroidBuffer *buffer, int acquire_fence_fd) {
    compositor::SurfaceFrame frame;
    frame.surface_id = surface_id_;
    frame.sequence = next_sequence_++;
    frame.buffer = WPEAndroidBuffer_getAHardwareBuffer(buffer);
    frame.buffer_width = width_;
    frame.buffer_height = height_;
    frame.acquire_fence_fd = acquire_fence_fd;
    frame.width = width_;
    frame.height = height_;
    auto pending = std::find_if(
        pending_buffers_.begin(), pending_buffers_.end(),
        [](const PendingBuffer &candidate) { return candidate.sequence == 0; });
    if (pending == pending_buffers_.end()) {
      if (acquire_fence_fd >= 0)
        close(acquire_fence_fd);
      WPEAndroidViewBackend_dispatchReleaseBuffer(backend_, buffer);
      WPEAndroidViewBackend_dispatchFrameComplete(backend_);
      return;
    }
    *pending = {frame.sequence, buffer};
    surface_sink_.submitSurface(frame, submissionComplete, this);
  }

  void complete(uint64_t sequence, bool presented) {
    const auto pending =
        std::find_if(pending_buffers_.begin(), pending_buffers_.end(),
                     [sequence](const PendingBuffer &candidate) {
                       return candidate.sequence == sequence;
                     });
    if (pending == pending_buffers_.end())
      return;
    WPEAndroidViewBackend_dispatchReleaseBuffer(backend_, pending->buffer);
    *pending = {};
    if (!presented) {
      if (trace_frames_) {
        std::fprintf(stderr,
                     "OOS compositor dropped WPE surface=%llu frame=%llu\n",
                     static_cast<unsigned long long>(surface_id_),
                     static_cast<unsigned long long>(sequence));
      }
    } else {
      ++presented_frames_;
      if (trace_frames_) {
        std::fprintf(
            stderr, "WPE surface producer released frame=%llu presented=%llu\n",
            static_cast<unsigned long long>(sequence),
            static_cast<unsigned long long>(presented_frames_));
        std::fflush(stderr);
      }
    }
    WPEAndroidViewBackend_dispatchFrameComplete(backend_);
  }

  compositor::SurfaceSink &surface_sink_;
  const uint64_t surface_id_;
  const uint32_t width_;
  const uint32_t height_;
  WPEAndroidViewBackend *backend_ = nullptr;
  struct PendingBuffer {
    uint64_t sequence = 0;
    WPEAndroidBuffer *buffer = nullptr;
  };
  uint64_t next_sequence_ = 1;
  uint64_t presented_frames_ = 0;
  std::array<PendingBuffer, 4> pending_buffers_{};
  bool trace_frames_ = false;
};

WpeSurfaceHost::WpeSurfaceHost(compositor::SurfaceSink &surface_sink,
                               uint64_t surface_id, uint32_t width,
                               uint32_t height)
    : impl_(std::make_unique<Impl>(surface_sink, surface_id, width, height)) {}

WpeSurfaceHost::~WpeSurfaceHost() {
  const bool trace = std::getenv("OOS_TRACE_WPE_FRAMES") &&
                     std::strcmp(std::getenv("OOS_TRACE_WPE_FRAMES"), "0") != 0;
  impl_.reset();
  if (trace)
    std::fprintf(stderr, "WPE surface owner shutdown: complete\n");
}

bool WpeSurfaceHost::initialize() { return impl_->initialize(); }

wpe_view_backend *WpeSurfaceHost::viewBackend() const {
  return impl_->viewBackend();
}

uint64_t WpeSurfaceHost::presentedFrames() const {
  return impl_->presentedFrames();
}

} // namespace oos::web
