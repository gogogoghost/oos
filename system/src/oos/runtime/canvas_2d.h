#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace oos::resources {
class FontAssetService;
}

namespace oos::runtime {

enum class Canvas2dOpcode : uint8_t {
  Clear = 0,
  FillRect = 1,
  StrokeRect = 2,
  FillText = 3,
  PushClip = 4,
  PopClip = 5,
};

struct Canvas2dCommand {
  uint8_t opcode = 0;
  uint8_t reserved[3] = {};
  float x = 0;
  float y = 0;
  float width = 0;
  float height = 0;
  float radius = 0;
  float line_width = 1;
  float font_size = 14;
  uint32_t rgba = 0;
  uint32_t text_offset = 0;
  uint32_t text_length = 0;
};

static_assert(sizeof(Canvas2dCommand) == 44,
              "Canvas2D command wire layout changed");

class Canvas2dRenderer {
public:
  Canvas2dRenderer(uint32_t width, uint32_t height,
                   resources::FontAssetService *fonts);
  ~Canvas2dRenderer();

  Canvas2dRenderer(const Canvas2dRenderer &) = delete;
  Canvas2dRenderer &operator=(const Canvas2dRenderer &) = delete;

  bool resize(uint32_t width, uint32_t height);
  bool render(const Canvas2dCommand *commands, size_t command_count,
              const uint8_t *text, size_t text_size);
  float measureText(const char *text, size_t text_size, float font_size);

  uint32_t width() const;
  uint32_t height() const;
  const uint8_t *pixels() const;
  size_t pixelBytes() const;
  const std::string &lastError() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::runtime
