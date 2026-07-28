#include "oos/web/wpe_surface_host.h"

#include "oos/compositor/surface.h"

#include <glib.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wpe/fdo.h>
#include <wpe/unstable/fdo-shm.h>

#include <cstdio>

namespace oos::web {
namespace {

GSourceFuncs kFrameSourceFunctions = {
    nullptr,
    nullptr,
    [](GSource *source, GSourceFunc callback, gpointer data) -> gboolean {
      if (g_source_get_ready_time(source) == -1)
        return G_SOURCE_CONTINUE;
      g_source_set_ready_time(source, -1);
      return callback(data);
    },
    nullptr,
    nullptr,
    nullptr,
};

} // namespace

class WpeSurfaceHost::Impl {
public:
  Impl(compositor::SurfaceSink &surface_sink, uint64_t surface_id,
       uint32_t width, uint32_t height)
      : surface_sink_(surface_sink), surface_id_(surface_id), width_(width),
        height_(height) {}

  ~Impl() {
    if (frame_source_) {
      g_source_destroy(frame_source_);
      g_source_unref(frame_source_);
    }
    if (backend_)
      wpe_view_backend_exportable_fdo_destroy(backend_);
  }

  bool initialize() {
    if (backend_)
      return true;
    if (!wpe_fdo_initialize_shm())
      return false;
    static const wpe_view_backend_exportable_fdo_client client = {
        nullptr, nullptr, exportShm, nullptr, nullptr};
    backend_ =
        wpe_view_backend_exportable_fdo_create(&client, this, width_, height_);
    if (backend_) {
      wpe_view_backend_add_activity_state(
          viewBackend(), wpe_view_activity_state_visible |
                             wpe_view_activity_state_focused |
                             wpe_view_activity_state_in_window);
      frame_source_ = g_source_new(&kFrameSourceFunctions, sizeof(GSource));
      g_source_set_priority(frame_source_, G_PRIORITY_DEFAULT);
      g_source_set_callback(
          frame_source_,
          [](gpointer data) -> gboolean {
            auto *self = static_cast<Impl *>(data);
            if (self->backend_) {
              wpe_view_backend_exportable_fdo_dispatch_frame_complete(
                  self->backend_);
            }
            return G_SOURCE_CONTINUE;
          },
          this, nullptr);
      g_source_attach(frame_source_, g_main_context_default());
      g_source_set_ready_time(frame_source_, -1);
    }
    return backend_ != nullptr;
  }

  wpe_view_backend *viewBackend() const {
    return backend_ ? wpe_view_backend_exportable_fdo_get_view_backend(backend_)
                    : nullptr;
  }

  uint64_t presentedFrames() const { return presented_frames_; }

private:
  static void exportShm(void *data, wpe_fdo_shm_exported_buffer *buffer) {
    static_cast<Impl *>(data)->present(buffer);
  }

  void present(wpe_fdo_shm_exported_buffer *buffer) {
    wl_shm_buffer *shm = wpe_fdo_shm_exported_buffer_get_shm_buffer(buffer);
    if (!shm || wl_shm_buffer_get_format(shm) != WL_SHM_FORMAT_ARGB8888) {
      std::fprintf(stderr, "OOS local WPE received an unsupported SHM frame\n");
      wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(
          backend_, buffer);
      scheduleFrameComplete();
      return;
    }
    wl_shm_buffer_begin_access(shm);
    const auto *pixels =
        static_cast<const uint8_t *>(wl_shm_buffer_get_data(shm));
    const int32_t width = wl_shm_buffer_get_width(shm);
    const int32_t height = wl_shm_buffer_get_height(shm);
    const int32_t stride = wl_shm_buffer_get_stride(shm);
    compositor::SurfaceFrame frame;
    frame.surface_id = surface_id_;
    frame.buffer_type = compositor::NativeBufferType::SharedMemoryArgb8888;
    frame.buffer = const_cast<uint8_t *>(pixels);
    frame.buffer_width = width;
    frame.buffer_height = height;
    frame.buffer_stride = stride;
    frame.width = width_;
    frame.height = height_;
    const bool presented = surface_sink_.presentSurface(frame);
    wl_shm_buffer_end_access(shm);
    wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(
        backend_, buffer);
    scheduleFrameComplete();
    if (!presented) {
      std::fprintf(stderr, "OOS compositor rejected local WPE surface %llu\n",
                   static_cast<unsigned long long>(surface_id_));
      return;
    }
    ++presented_frames_;
  }

  void scheduleFrameComplete() {
    const gint64 now = g_get_monotonic_time();
    if (!last_frame_time_us_)
      last_frame_time_us_ = now;
    const gint64 next_frame = last_frame_time_us_ + G_USEC_PER_SEC / 60;
    last_frame_time_us_ = now;
    g_source_set_ready_time(frame_source_, next_frame <= now ? 0 : next_frame);
  }

  compositor::SurfaceSink &surface_sink_;
  const uint64_t surface_id_;
  const uint32_t width_;
  const uint32_t height_;
  wpe_view_backend_exportable_fdo *backend_ = nullptr;
  GSource *frame_source_ = nullptr;
  gint64 last_frame_time_us_ = 0;
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
