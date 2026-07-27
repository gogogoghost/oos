#pragma once

#include <cstdint>
#include <memory>

#include "oos/device/display.h"

namespace oos::nokia2780 {

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
  bool setTexture(uint32_t texture, uint32_t x, uint32_t y, uint32_t width,
                  uint32_t height, uint32_t flags, const uint8_t *rgba,
                  size_t rgba_size) override;
  bool freeTexture(uint32_t texture) override;
  bool submit(const OosGfxVertex *vertices, size_t vertex_count,
              const uint16_t *indices, size_t index_count,
              const OosGfxDrawCommand *commands, size_t command_count,
              uint32_t clear_rgba) override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::nokia2780
