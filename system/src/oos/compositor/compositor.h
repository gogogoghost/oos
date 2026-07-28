#pragma once

#include "oos/compositor/surface.h"
#include "oos/runtime/graphics_host.h"

namespace oos::device {
class Display;
}

namespace oos::compositor {

// OOS owns the only route to the physical display. Native/WASM drawing and
// external producers such as WPE all submit through this host compositor.
class Compositor final : public SurfaceSink, public runtime::GraphicsHost {
public:
  explicit Compositor(device::Display &display);

  bool presentSurface(const SurfaceFrame &frame) override;

  uint32_t width() const override;
  uint32_t height() const override;
  bool setTexture(uint32_t texture, uint32_t x, uint32_t y, uint32_t width,
                  uint32_t height, uint32_t flags, const uint8_t *rgba,
                  size_t rgba_size) override;
  bool freeTexture(uint32_t texture) override;
  bool submit(const OosGfxVertex *vertices, size_t vertex_count,
              const uint16_t *indices, size_t index_count,
              const OosGfxDrawCommand *commands, size_t command_count,
              uint32_t clear_rgba) override;

private:
  device::Display &display_;
};

} // namespace oos::compositor
