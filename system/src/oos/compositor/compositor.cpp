#include "oos/compositor/compositor.h"

#include "oos/device/display.h"

#include <cmath>
#include <unistd.h>

namespace oos::compositor {

Compositor::Compositor(device::Display &display) : display_(display) {}

bool Compositor::presentSurface(const SurfaceFrame &frame) {
  const bool valid =
      frame.surface_id != 0 && frame.buffer != nullptr &&
      frame.buffer_width != 0 && frame.buffer_height != 0 && frame.x == 0 &&
      frame.y == 0 && frame.width == width() && frame.height == height() &&
      frame.buffer_width == frame.width &&
      frame.buffer_height == frame.height && std::isfinite(frame.opacity) &&
      frame.opacity == 1.0f && frame.z_order == 0;
  if (!valid) {
    if (frame.acquire_fence_fd >= 0)
      close(frame.acquire_fence_fd);
    return false;
  }
  return display_.presentSurface(frame);
}

uint32_t Compositor::width() const { return display_.width(); }

uint32_t Compositor::height() const { return display_.height(); }

bool Compositor::setTexture(uint32_t texture, uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height, uint32_t flags,
                            const uint8_t *rgba, size_t rgba_size) {
  return display_.setTexture(texture, x, y, width, height, flags, rgba,
                             rgba_size);
}

bool Compositor::freeTexture(uint32_t texture) {
  return display_.freeTexture(texture);
}

bool Compositor::submit(const OosGfxVertex *vertices, size_t vertex_count,
                        const uint16_t *indices, size_t index_count,
                        const OosGfxDrawCommand *commands, size_t command_count,
                        uint32_t clear_rgba) {
  return display_.submit(vertices, vertex_count, indices, index_count, commands,
                         command_count, clear_rgba);
}

} // namespace oos::compositor
