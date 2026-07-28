#pragma once

#include <cstdint>

namespace oos::compositor {

enum class NativeBufferType : uint8_t {
  AndroidHardwareBuffer,
  SharedMemoryArgb8888,
};

// The producer retains ownership until presentSurface() returns. The acquire
// fence, when present, is consumed by the display backend.
struct SurfaceFrame {
  uint64_t surface_id = 0;
  NativeBufferType buffer_type = NativeBufferType::AndroidHardwareBuffer;
  void *buffer = nullptr;
  uint32_t buffer_width = 0;
  uint32_t buffer_height = 0;
  uint32_t buffer_stride = 0;
  int acquire_fence_fd = -1;
  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  float opacity = 1.0f;
  int32_t z_order = 0;
};

class SurfaceSink {
public:
  virtual ~SurfaceSink() = default;
  virtual bool presentSurface(const SurfaceFrame &frame) = 0;
};

} // namespace oos::compositor
