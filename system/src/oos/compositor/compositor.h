#pragma once

#include "oos/compositor/surface.h"
#include "oos/runtime/graphics_host.h"

#include <memory>
#include <string>

namespace oos::device {
class Display;
}

namespace oos::compositor {

struct LayerSurfaceConfig {
  std::string name;
  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  int32_t z_order = 0;
};

class LayerSurface;

// OOS owns the only route to the physical display. Native guests and future
// out-of-process producers submit through this host compositor.
class Compositor final : public SurfaceSink, public runtime::GraphicsHost {
public:
  explicit Compositor(device::Display &display);
  ~Compositor();

  std::unique_ptr<LayerSurface> createLayer(const LayerSurfaceConfig &config);
  bool compose();
  bool dirty() const;

  bool presentSurface(const SurfaceFrame &frame) override;
  bool detachRenderContext();
  bool attachRenderContext();

  uint32_t width() const override;
  uint32_t height() const override;
  uint32_t surfaceFormat() const override;
  uint32_t supportedTextureFormats() const override;
  bool setTexture(uint32_t texture, uint32_t format, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height, uint32_t row_stride,
                  uint32_t flags, const uint8_t *pixels,
                  size_t pixel_bytes) override;
  bool freeTexture(uint32_t texture) override;
  bool submit(const OosGfxVertex *vertices, size_t vertex_count,
              const uint16_t *indices, size_t index_count,
              const OosGfxDrawCommand *commands, size_t command_count,
              uint32_t clear_rgba) override;
  bool glesCapabilities(OosGlesCapabilities &result) override;
  bool setGlesBuffer(uint32_t buffer, uint32_t size, uint32_t usage,
                     const uint8_t *data, size_t data_size) override;
  bool writeGlesBuffer(uint32_t buffer, uint32_t offset, const uint8_t *data,
                       size_t data_size) override;
  bool freeGlesBuffer(uint32_t buffer) override;
  bool setGlesShader(uint32_t shader, uint32_t stage, const char *source,
                     size_t source_size) override;
  bool freeGlesShader(uint32_t shader) override;
  bool setGlesProgram(uint32_t program, uint32_t vertex_shader,
                      uint32_t fragment_shader) override;
  bool freeGlesProgram(uint32_t program) override;
  int32_t glesAttributeLocation(uint32_t program, const char *name,
                                size_t name_size) override;
  int32_t glesUniformLocation(uint32_t program, const char *name,
                              size_t name_size) override;
  bool submitGles(const OosGlesCommand *commands, size_t command_count,
                  const uint32_t *data, size_t data_words) override;

private:
  friend class LayerSurface;
  class Impl;

  device::Display &display_;
  std::unique_ptr<Impl> impl_;
};

// A producer-scoped graphics endpoint. Texture handles and retained draw
// batches are isolated per layer; only Compositor can submit the merged scene
// to the physical display.
class LayerSurface final : public runtime::GraphicsHost {
public:
  ~LayerSurface() override;

  LayerSurface(const LayerSurface &) = delete;
  LayerSurface &operator=(const LayerSurface &) = delete;

  const std::string &name() const;
  int32_t zOrder() const;
  void setVisible(bool visible);
  bool visible() const;
  bool setGeometry(int32_t x, int32_t y, uint32_t width, uint32_t height);
  void clearFrame();

  uint32_t width() const override;
  uint32_t height() const override;
  uint32_t surfaceFormat() const override;
  uint32_t supportedTextureFormats() const override;
  bool setTexture(uint32_t texture, uint32_t format, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height, uint32_t row_stride,
                  uint32_t flags, const uint8_t *pixels,
                  size_t pixel_bytes) override;
  bool freeTexture(uint32_t texture) override;
  bool submit(const OosGfxVertex *vertices, size_t vertex_count,
              const uint16_t *indices, size_t index_count,
              const OosGfxDrawCommand *commands, size_t command_count,
              uint32_t clear_rgba) override;
  bool glesCapabilities(OosGlesCapabilities &result) override;
  bool setGlesBuffer(uint32_t, uint32_t, uint32_t, const uint8_t *,
                     size_t) override;
  bool writeGlesBuffer(uint32_t, uint32_t, const uint8_t *, size_t) override;
  bool freeGlesBuffer(uint32_t) override;
  bool setGlesShader(uint32_t, uint32_t, const char *, size_t) override;
  bool freeGlesShader(uint32_t) override;
  bool setGlesProgram(uint32_t, uint32_t, uint32_t) override;
  bool freeGlesProgram(uint32_t) override;
  int32_t glesAttributeLocation(uint32_t, const char *, size_t) override;
  int32_t glesUniformLocation(uint32_t, const char *, size_t) override;
  bool submitGles(const OosGlesCommand *, size_t, const uint32_t *,
                  size_t) override;

private:
  friend class Compositor;
  class Impl;

  LayerSurface(Compositor &compositor, const LayerSurfaceConfig &config,
               uint64_t id);
  Compositor &compositor_;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::compositor
