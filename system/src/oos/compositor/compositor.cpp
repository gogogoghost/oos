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

bool Compositor::detachRenderContext() {
  return display_.detachRenderContext();
}

bool Compositor::attachRenderContext() {
  return display_.attachRenderContext();
}

uint32_t Compositor::width() const { return display_.width(); }

uint32_t Compositor::height() const { return display_.height(); }

uint32_t Compositor::surfaceFormat() const { return display_.surfaceFormat(); }

uint32_t Compositor::supportedTextureFormats() const {
  return display_.supportedTextureFormats();
}

bool Compositor::setTexture(uint32_t texture, uint32_t format, uint32_t x,
                            uint32_t y, uint32_t width, uint32_t height,
                            uint32_t row_stride, uint32_t flags,
                            const uint8_t *pixels, size_t pixel_bytes) {
  return display_.setTexture(texture, format, x, y, width, height, row_stride,
                             flags, pixels, pixel_bytes);
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

bool Compositor::glesCapabilities(OosGlesCapabilities &result) {
  return display_.glesCapabilities(result);
}

bool Compositor::setGlesBuffer(uint32_t buffer, uint32_t size, uint32_t usage,
                               const uint8_t *data, size_t data_size) {
  return display_.setGlesBuffer(buffer, size, usage, data, data_size);
}

bool Compositor::writeGlesBuffer(uint32_t buffer, uint32_t offset,
                                 const uint8_t *data, size_t data_size) {
  return display_.writeGlesBuffer(buffer, offset, data, data_size);
}

bool Compositor::freeGlesBuffer(uint32_t buffer) {
  return display_.freeGlesBuffer(buffer);
}

bool Compositor::setGlesShader(uint32_t shader, uint32_t stage,
                               const char *source, size_t source_size) {
  return display_.setGlesShader(shader, stage, source, source_size);
}

bool Compositor::freeGlesShader(uint32_t shader) {
  return display_.freeGlesShader(shader);
}

bool Compositor::setGlesProgram(uint32_t program, uint32_t vertex_shader,
                                uint32_t fragment_shader) {
  return display_.setGlesProgram(program, vertex_shader, fragment_shader);
}

bool Compositor::freeGlesProgram(uint32_t program) {
  return display_.freeGlesProgram(program);
}

int32_t Compositor::glesAttributeLocation(uint32_t program, const char *name,
                                          size_t name_size) {
  return display_.glesAttributeLocation(program, name, name_size);
}

int32_t Compositor::glesUniformLocation(uint32_t program, const char *name,
                                        size_t name_size) {
  return display_.glesUniformLocation(program, name, name_size);
}

bool Compositor::submitGles(const OosGlesCommand *commands,
                            size_t command_count, const uint32_t *data,
                            size_t data_words) {
  return display_.submitGles(commands, command_count, data, data_words);
}

} // namespace oos::compositor
