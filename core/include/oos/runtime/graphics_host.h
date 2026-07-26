#pragma once

#include <cstddef>
#include <cstdint>

#include "oos/wasm/abi.h"

namespace oos::runtime {

class GraphicsHost {
public:
  virtual ~GraphicsHost() = default;

  virtual uint32_t width() const = 0;
  virtual uint32_t height() const = 0;

  virtual bool setTexture(uint32_t texture, uint32_t x, uint32_t y,
                          uint32_t width, uint32_t height, uint32_t flags,
                          const uint8_t *rgba, size_t rgba_size) = 0;
  virtual bool freeTexture(uint32_t texture) = 0;
  virtual bool submit(const OosGfxVertex *vertices, size_t vertex_count,
                      const uint16_t *indices, size_t index_count,
                      const OosGfxDrawCommand *commands, size_t command_count,
                      uint32_t clear_rgba) = 0;
};

} // namespace oos::runtime
