#pragma once

#include "oos/device/device.h"
#include "oos/runtime/graphics_host.h"
#include "oos/ui/status_bar_appearance.h"

#include <string>

namespace oos::sdk::guest {

class GraphicsHost : public runtime::GraphicsHost {
public:
  GraphicsHost();

  uint32_t width() const override;
  uint32_t height() const override;
  float pixelsPerPoint() const override;
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
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

class StatusBarController final
    : public ::oos::ui::StatusBarAppearanceController {
public:
  void setStatusBarAppearance(
      ::oos::ui::StatusBarAppearance appearance) override;
  ::oos::ui::StatusBarAppearance statusBarAppearance() const override;
  bool setSurfaceMode(::oos::ui::SurfaceMode mode) override;
  ::oos::ui::SurfaceMode surfaceMode() const override;

private:
  ::oos::ui::StatusBarAppearance appearance_{};
  ::oos::ui::SurfaceMode mode_ = ::oos::ui::SurfaceMode::Normal;
};

class Device final : public device::Device {
public:
  Device();

  const device::DeviceDescriptor &descriptor() const override;
  const device::ServiceConfiguration &services() const override;
  device::CapabilityState capability(device::Feature feature) const override;
  bool initialize(const device::DeviceInitOptions &options = {}) override;
  void shutdown() override;
  device::Display &display() override;
  input::KeyInputSource &keyInput() override;
  const std::string &lastError() const override;

private:
  device::DeviceDescriptor descriptor_{};
  device::ServiceConfiguration services_{};
  std::string id_;
  std::string manufacturer_;
  std::string model_;
  std::string cpu_core_;
  std::string cpu_arch_;
  std::string error_;
};

int64_t wallClockSeconds();
uint32_t wallClockMinute();

} // namespace oos::sdk::guest
