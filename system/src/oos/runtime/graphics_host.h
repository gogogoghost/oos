#pragma once

#include <cstddef>
#include <cstdint>

#include "oos/runtime/graphics_types.h"

namespace oos::runtime {

class GraphicsHost {
public:
  virtual ~GraphicsHost() = default;

  virtual uint32_t width() const = 0;
  virtual uint32_t height() const = 0;
  virtual uint32_t surfaceFormat() const = 0;
  virtual uint32_t supportedTextureFormats() const = 0;

  virtual bool setTexture(uint32_t texture, uint32_t format, uint32_t x,
                          uint32_t y, uint32_t width, uint32_t height,
                          uint32_t row_stride, uint32_t flags,
                          const uint8_t *pixels, size_t pixel_bytes) = 0;
  virtual bool freeTexture(uint32_t texture) = 0;
  virtual bool submit(const OosGfxVertex *vertices, size_t vertex_count,
                      const uint16_t *indices, size_t index_count,
                      const OosGfxDrawCommand *commands, size_t command_count,
                      uint32_t clear_rgba) = 0;

  virtual bool glesCapabilities(OosGlesCapabilities &result) = 0;
  virtual bool setGlesBuffer(uint32_t buffer, uint32_t size, uint32_t usage,
                             const uint8_t *data, size_t data_size) = 0;
  virtual bool writeGlesBuffer(uint32_t buffer, uint32_t offset,
                               const uint8_t *data, size_t data_size) = 0;
  virtual bool freeGlesBuffer(uint32_t buffer) = 0;
  virtual bool setGlesShader(uint32_t shader, uint32_t stage,
                             const char *source, size_t source_size) = 0;
  virtual bool freeGlesShader(uint32_t shader) = 0;
  virtual bool setGlesProgram(uint32_t program, uint32_t vertex_shader,
                              uint32_t fragment_shader) = 0;
  virtual bool freeGlesProgram(uint32_t program) = 0;
  virtual int32_t glesAttributeLocation(uint32_t program, const char *name,
                                        size_t name_size) = 0;
  virtual int32_t glesUniformLocation(uint32_t program, const char *name,
                                      size_t name_size) = 0;
  virtual bool submitGles(const OosGlesCommand *commands, size_t command_count,
                          const uint32_t *data, size_t data_words) = 0;
};

} // namespace oos::runtime
