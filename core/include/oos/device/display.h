#pragma once

#include "oos/compositor/surface.h"
#include "oos/runtime/graphics_host.h"

#include <cstdint>

namespace oos::device {

// Device-independent primary display lifecycle. Rendering for native apps is
// inherited from GraphicsHost; panel/HWC ownership remains in the backend.
class Display : public runtime::GraphicsHost {
public:
  ~Display() override = default;

  virtual bool initialize() = 0;
  virtual bool showBootFrame(const uint16_t *rgb565_pixels) = 0;
  virtual bool presentSurface(const compositor::SurfaceFrame &frame) = 0;
  virtual void refresh() = 0;
  virtual void shutdown() = 0;
};

} // namespace oos::device
