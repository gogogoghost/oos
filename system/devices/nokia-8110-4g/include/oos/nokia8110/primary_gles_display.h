#pragma once

#include <cstdint>
#include <memory>

#include "oos/device/display.h"

namespace oos::nokia8110 {

class PrimaryGlesDisplay final : public device::Display {
public:
  static constexpr uint32_t kWidth = 240;
  static constexpr uint32_t kHeight = 320;

  PrimaryGlesDisplay();
  ~PrimaryGlesDisplay() override;

  PrimaryGlesDisplay(const PrimaryGlesDisplay &) = delete;
  PrimaryGlesDisplay &operator=(const PrimaryGlesDisplay &) = delete;

  bool initialize() override;
  bool showBootFrame(const uint16_t *rgb565_pixels) override;
  bool presentSurface(const compositor::SurfaceFrame &frame) override;
  void refresh() override;
  void shutdown() override;

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
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::nokia8110
