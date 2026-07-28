#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "oos/runtime/graphics_types.h"

namespace oos::runtime {

// Device backends keep ownership of the render target and presentation path.
// The executor can only bind this target; guest commands cannot select a
// native framebuffer or escape host composition.
class GlesFrameTarget {
public:
  virtual ~GlesFrameTarget() = default;

  virtual bool makeGlesContextCurrent() = 0;
  virtual bool bindGlesSurface(bool require_depth, bool require_stencil) = 0;
  virtual bool presentGlesSurface() = 0;
  virtual uint32_t glesSurfaceWidth() const = 0;
  virtual uint32_t glesSurfaceHeight() const = 0;
  virtual uint32_t glesDepthBits() const = 0;
  virtual uint32_t glesStencilBits() const = 0;
};

class GlesExecutor {
public:
  explicit GlesExecutor(GlesFrameTarget &target);
  ~GlesExecutor();

  GlesExecutor(const GlesExecutor &) = delete;
  GlesExecutor &operator=(const GlesExecutor &) = delete;

  bool capabilities(OosGlesCapabilities &result);

  bool setTexture(uint32_t texture, uint32_t format, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height, uint32_t row_stride,
                  uint32_t flags, const uint8_t *pixels, size_t pixel_bytes);
  bool freeTexture(uint32_t texture);
  uint32_t textureName(uint32_t texture) const;
  uint32_t textureFormat(uint32_t texture) const;

  bool setBuffer(uint32_t buffer, uint32_t size, uint32_t usage,
                 const uint8_t *data, size_t data_size);
  bool writeBuffer(uint32_t buffer, uint32_t offset, const uint8_t *data,
                   size_t data_size);
  bool freeBuffer(uint32_t buffer);

  bool setShader(uint32_t shader, uint32_t stage, const char *source,
                 size_t source_size);
  bool freeShader(uint32_t shader);
  bool setProgram(uint32_t program, uint32_t vertex_shader,
                  uint32_t fragment_shader);
  bool freeProgram(uint32_t program);
  int32_t attributeLocation(uint32_t program, const char *name,
                            size_t name_size);
  int32_t uniformLocation(uint32_t program, const char *name, size_t name_size);

  bool submit(const OosGlesCommand *commands, size_t command_count,
              const uint32_t *data, size_t data_words);
  void reset();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::runtime
