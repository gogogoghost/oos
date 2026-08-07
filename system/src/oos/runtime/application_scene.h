#pragma once

#include "oos/runtime/canvas_2d.h"
#include "oos/runtime/graphics_host.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace oos::runtime {

enum class CanvasContextKind : uint8_t {
  None = 0,
  Canvas2d = 1,
  Mesh2d = 2,
  Gles2 = 3,
};

struct CanvasGeometry {
  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  int32_t z_order = 0;
  bool visible = true;
};

// One application-owned scene. Framework UI and explicit canvases are retained
// as sibling nodes and flattened to one compositor submission at frame end.
class ApplicationScene final : public GraphicsHost {
public:
  explicit ApplicationScene(GraphicsHost &target,
                            std::string font_directory =
                                "/opt/oos/share/fonts");
  ~ApplicationScene() override;

  ApplicationScene(const ApplicationScene &) = delete;
  ApplicationScene &operator=(const ApplicationScene &) = delete;

  uint32_t createCanvas(const CanvasGeometry &geometry,
                        CanvasContextKind context);
  bool configureCanvas(uint32_t canvas, const CanvasGeometry &geometry);
  // Updates CSS/layout placement without resizing the canvas drawing buffer.
  bool placeCanvas(uint32_t canvas, const CanvasGeometry &geometry);
  bool destroyCanvas(uint32_t canvas);
  bool submitCanvas2d(uint32_t canvas, const Canvas2dCommand *commands,
                      size_t command_count, const uint8_t *text,
                      size_t text_size);
  bool setCanvasTexture(uint32_t canvas, uint32_t texture, uint32_t format,
                        uint32_t x, uint32_t y, uint32_t width,
                        uint32_t height, uint32_t row_stride, uint32_t flags,
                        const uint8_t *pixels, size_t pixel_bytes);
  bool freeCanvasTexture(uint32_t canvas, uint32_t texture);
  bool submitCanvasMesh(uint32_t canvas, const OosGfxVertex *vertices,
                        size_t vertex_count, const uint16_t *indices,
                        size_t index_count,
                        const OosGfxDrawCommand *commands,
                        size_t command_count, uint32_t clear_rgba);
  bool canvasGlesCapabilities(uint32_t canvas, OosGlesCapabilities &result);
  bool setCanvasGlesTexture(uint32_t canvas, uint32_t texture,
                            uint32_t format, uint32_t x, uint32_t y,
                            uint32_t width, uint32_t height,
                            uint32_t row_stride, uint32_t flags,
                            const uint8_t *pixels, size_t pixel_bytes);
  bool freeCanvasGlesTexture(uint32_t canvas, uint32_t texture);
  bool setCanvasGlesBuffer(uint32_t canvas, uint32_t buffer, uint32_t size,
                           uint32_t usage, const uint8_t *data,
                           size_t data_size);
  bool writeCanvasGlesBuffer(uint32_t canvas, uint32_t buffer,
                             uint32_t offset, const uint8_t *data,
                             size_t data_size);
  bool freeCanvasGlesBuffer(uint32_t canvas, uint32_t buffer);
  bool setCanvasGlesShader(uint32_t canvas, uint32_t shader, uint32_t stage,
                           const char *source, size_t source_size);
  bool freeCanvasGlesShader(uint32_t canvas, uint32_t shader);
  bool setCanvasGlesProgram(uint32_t canvas, uint32_t program,
                            uint32_t vertex_shader,
                            uint32_t fragment_shader);
  bool freeCanvasGlesProgram(uint32_t canvas, uint32_t program);
  int32_t canvasGlesAttributeLocation(uint32_t canvas, uint32_t program,
                                      const char *name, size_t name_size);
  int32_t canvasGlesUniformLocation(uint32_t canvas, uint32_t program,
                                    const char *name, size_t name_size);
  bool submitCanvasGles(uint32_t canvas, const OosGlesCommand *commands,
                        size_t command_count, const uint32_t *data,
                        size_t data_words);
  bool present();
  float measureText(const char *text, size_t text_size, float font_size);
  void reset();

  const std::string &lastError() const;

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
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::runtime
