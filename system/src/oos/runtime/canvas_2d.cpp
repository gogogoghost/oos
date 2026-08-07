#include "oos/runtime/canvas_2d.h"

#include "oos/resources/font_assets.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oos::runtime {
namespace {

constexpr uint64_t kMaximumCanvasPixels = 4 * 1024 * 1024;
constexpr size_t kMaximumCommands = 8192;
constexpr size_t kMaximumTextBytes = 1024 * 1024;

bool finite(float value) { return std::isfinite(value); }

uint32_t nextCodepoint(const uint8_t *bytes, size_t size, size_t &offset) {
  if (offset >= size)
    return 0;
  const uint8_t first = bytes[offset++];
  if (first < 0x80)
    return first;
  uint32_t value = 0;
  size_t continuation = 0;
  if ((first & 0xe0) == 0xc0) {
    value = first & 0x1f;
    continuation = 1;
  } else if ((first & 0xf0) == 0xe0) {
    value = first & 0x0f;
    continuation = 2;
  } else if ((first & 0xf8) == 0xf0) {
    value = first & 0x07;
    continuation = 3;
  } else {
    return 0xfffd;
  }
  if (continuation > size - offset)
    return 0xfffd;
  for (size_t index = 0; index < continuation; ++index) {
    const uint8_t byte = bytes[offset++];
    if ((byte & 0xc0) != 0x80)
      return 0xfffd;
    value = (value << 6) | (byte & 0x3f);
  }
  if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
    return 0xfffd;
  return value;
}

struct FontFace {
  std::vector<uint8_t> bytes;
  stbtt_fontinfo info = {};
  bool ready = false;
};

bool loadFace(resources::FontAssetService *service, resources::FontRole role,
              FontFace &face) {
  if (!service)
    return false;
  uint64_t size = 0;
  if (service->fileSize(role, size) != resources::FontAssetStatus::Ok ||
      size == 0 || size > std::numeric_limits<size_t>::max())
    return false;
  face.bytes.resize(static_cast<size_t>(size));
  size_t read = 0;
  if (service->readInto(role, face.bytes.data(), face.bytes.size(), read) !=
          resources::FontAssetStatus::Ok ||
      read != face.bytes.size()) {
    face.bytes.clear();
    return false;
  }
  const int offset = stbtt_GetFontOffsetForIndex(face.bytes.data(), 0);
  face.ready = offset >= 0 &&
               stbtt_InitFont(&face.info, face.bytes.data(), offset) != 0;
  if (!face.ready)
    face.bytes.clear();
  return face.ready;
}

} // namespace

class Canvas2dRenderer::Impl {
public:
  struct ClipRect {
    int left;
    int top;
    int right;
    int bottom;
  };

  struct GlyphKey {
    uint32_t codepoint;
    uint16_t pixels;
    uint8_t face;

    bool operator==(const GlyphKey &other) const {
      return codepoint == other.codepoint && pixels == other.pixels &&
             face == other.face;
    }
  };

  struct GlyphKeyHash {
    size_t operator()(const GlyphKey &key) const {
      return (static_cast<size_t>(key.codepoint) * 1315423911u) ^
             (static_cast<size_t>(key.pixels) << 8) ^ key.face;
    }
  };

  struct Glyph {
    std::vector<uint8_t> alpha;
    int width = 0;
    int height = 0;
    int x_offset = 0;
    int y_offset = 0;
    float advance = 0;
  };

  Impl(uint32_t width, uint32_t height,
       resources::FontAssetService *font_service)
      : fonts(font_service) {
    resize(width, height);
  }

  bool resize(uint32_t next_width, uint32_t next_height) {
    error.clear();
    if (next_width == 0 || next_height == 0 ||
        static_cast<uint64_t>(next_width) * next_height > kMaximumCanvasPixels) {
      error = "Canvas2D dimensions exceed the host limit";
      return false;
    }
    try {
      pixels.assign(static_cast<size_t>(next_width) * next_height * 4, 0);
    } catch (const std::bad_alloc &) {
      error = "allocate Canvas2D pixels failed";
      return false;
    }
    width = next_width;
    height = next_height;
    return true;
  }

  FontFace *faceFor(uint32_t codepoint, uint8_t &face_index) {
    ensureFonts();
    if (primary.ready && stbtt_FindGlyphIndex(&primary.info, codepoint) != 0) {
      face_index = 0;
      return &primary;
    }
    if (fallback.ready &&
        stbtt_FindGlyphIndex(&fallback.info, codepoint) != 0) {
      face_index = 1;
      return &fallback;
    }
    if (primary.ready) {
      face_index = 0;
      return &primary;
    }
    return nullptr;
  }

  void ensureFonts() {
    if (fonts_attempted)
      return;
    fonts_attempted = true;
    loadFace(fonts, resources::FontRole::UiProportional, primary);
    loadFace(fonts, resources::FontRole::CjkFallback, fallback);
  }

  const Glyph *glyph(uint32_t codepoint, float font_size) {
    const uint16_t pixels = static_cast<uint16_t>(
        std::clamp(std::lround(font_size), 6l, 96l));
    uint8_t face_index = 0;
    FontFace *face = faceFor(codepoint, face_index);
    if (!face)
      return nullptr;
    const GlyphKey key{codepoint, pixels, face_index};
    const auto found = glyphs.find(key);
    if (found != glyphs.end())
      return &found->second;
    const float scale = stbtt_ScaleForPixelHeight(&face->info, pixels);
    Glyph value;
    int advance = 0;
    int bearing = 0;
    stbtt_GetCodepointHMetrics(&face->info, codepoint, &advance, &bearing);
    value.advance = advance * scale;
    unsigned char *bitmap = stbtt_GetCodepointBitmap(
        &face->info, scale, scale, codepoint, &value.width, &value.height,
        &value.x_offset, &value.y_offset);
    if (bitmap && value.width > 0 && value.height > 0) {
      value.alpha.assign(bitmap, bitmap +
                                     static_cast<size_t>(value.width) *
                                         value.height);
    }
    if (bitmap)
      stbtt_FreeBitmap(bitmap, nullptr);
    return &glyphs.emplace(key, std::move(value)).first->second;
  }

  void blend(int x, int y, uint8_t red, uint8_t green, uint8_t blue,
             uint8_t alpha) {
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= width ||
        static_cast<uint32_t>(y) >= height || x < clip.left || x >= clip.right ||
        y < clip.top || y >= clip.bottom || alpha == 0)
      return;
    uint8_t *destination =
        pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
    const uint32_t inverse = 255 - alpha;
    const uint32_t source_red = (static_cast<uint32_t>(red) * alpha + 127) / 255;
    const uint32_t source_green =
        (static_cast<uint32_t>(green) * alpha + 127) / 255;
    const uint32_t source_blue =
        (static_cast<uint32_t>(blue) * alpha + 127) / 255;
    destination[0] = static_cast<uint8_t>(
        std::min(255u, source_red + (destination[0] * inverse + 127) / 255));
    destination[1] = static_cast<uint8_t>(
        std::min(255u, source_green + (destination[1] * inverse + 127) / 255));
    destination[2] = static_cast<uint8_t>(
        std::min(255u, source_blue + (destination[2] * inverse + 127) / 255));
    destination[3] = static_cast<uint8_t>(
        std::min(255u, static_cast<uint32_t>(alpha) +
                           (destination[3] * inverse + 127) / 255));
  }

  void fillRect(float left, float top, float rect_width, float rect_height,
                float radius, uint32_t rgba) {
    if (!finite(left) || !finite(top) || !finite(rect_width) ||
        !finite(rect_height) || !finite(radius) || rect_width <= 0 ||
        rect_height <= 0)
      return;
    const int x0 = std::max(0, static_cast<int>(std::floor(left)));
    const int y0 = std::max(0, static_cast<int>(std::floor(top)));
    const int x1 = std::min(static_cast<int>(width),
                            static_cast<int>(std::ceil(left + rect_width)));
    const int y1 = std::min(static_cast<int>(height),
                            static_cast<int>(std::ceil(top + rect_height)));
    const float corner = std::clamp(radius, 0.0f,
                                    std::min(rect_width, rect_height) * 0.5f);
    const uint8_t red = static_cast<uint8_t>(rgba);
    const uint8_t green = static_cast<uint8_t>(rgba >> 8);
    const uint8_t blue = static_cast<uint8_t>(rgba >> 16);
    const uint8_t alpha = static_cast<uint8_t>(rgba >> 24);
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        if (corner > 0) {
          const float px = x + 0.5f;
          const float py = y + 0.5f;
          const float nearest_x =
              std::clamp(px, left + corner, left + rect_width - corner);
          const float nearest_y =
              std::clamp(py, top + corner, top + rect_height - corner);
          const float dx = px - nearest_x;
          const float dy = py - nearest_y;
          if (dx * dx + dy * dy > corner * corner)
            continue;
        }
        blend(x, y, red, green, blue, alpha);
      }
    }
  }

  void strokeRect(const Canvas2dCommand &command) {
    const float thickness = std::max(1.0f, command.line_width);
    fillRect(command.x, command.y, command.width, thickness, command.radius,
             command.rgba);
    fillRect(command.x, command.y + command.height - thickness, command.width,
             thickness, command.radius, command.rgba);
    fillRect(command.x, command.y + thickness, thickness,
             command.height - thickness * 2, 0, command.rgba);
    fillRect(command.x + command.width - thickness,
             command.y + thickness, thickness,
             command.height - thickness * 2, 0, command.rgba);
  }

  void fillText(const Canvas2dCommand &command, const uint8_t *text) {
    float cursor = command.x;
    size_t offset = 0;
    while (offset < command.text_length) {
      const uint32_t codepoint =
          nextCodepoint(text, command.text_length, offset);
      const Glyph *value = glyph(codepoint, command.font_size);
      if (!value)
        continue;
      const int origin_x = static_cast<int>(std::floor(cursor)) + value->x_offset;
      const int origin_y = static_cast<int>(std::floor(command.y)) + value->y_offset;
      const uint8_t red = static_cast<uint8_t>(command.rgba);
      const uint8_t green = static_cast<uint8_t>(command.rgba >> 8);
      const uint8_t blue = static_cast<uint8_t>(command.rgba >> 16);
      const uint8_t base_alpha = static_cast<uint8_t>(command.rgba >> 24);
      for (int y = 0; y < value->height; ++y) {
        for (int x = 0; x < value->width; ++x) {
          const uint8_t coverage =
              value->alpha[static_cast<size_t>(y) * value->width + x];
          const uint8_t alpha = static_cast<uint8_t>(
              (static_cast<uint32_t>(coverage) * base_alpha + 127) / 255);
          blend(origin_x + x, origin_y + y, red, green, blue, alpha);
        }
      }
      cursor += value->advance;
      if (command.width > 0 && cursor > command.x + command.width)
        break;
    }
  }

  bool render(const Canvas2dCommand *commands, size_t command_count,
              const uint8_t *text, size_t text_size) {
    error.clear();
    if ((!commands && command_count != 0) || command_count > kMaximumCommands ||
        (!text && text_size != 0) || text_size > kMaximumTextBytes) {
      error = "Canvas2D batch exceeds the host limit";
      return false;
    }
    size_t clip_depth = 0;
    for (size_t index = 0; index < command_count; ++index) {
      const Canvas2dCommand &command = commands[index];
      if (command.opcode > static_cast<uint8_t>(Canvas2dOpcode::PopClip) ||
          !finite(command.x) || !finite(command.y) || !finite(command.width) ||
          !finite(command.height) || !finite(command.radius) ||
          !finite(command.line_width) || !finite(command.font_size) ||
          command.text_offset > text_size ||
          command.text_length > text_size - command.text_offset) {
        error = "Canvas2D command is invalid";
        return false;
      }
      if (command.opcode == static_cast<uint8_t>(Canvas2dOpcode::PushClip)) {
        if (++clip_depth > 64) {
          error = "Canvas2D clip nesting exceeds the host limit";
          return false;
        }
      } else if (command.opcode ==
                 static_cast<uint8_t>(Canvas2dOpcode::PopClip)) {
        if (clip_depth == 0) {
          error = "Canvas2D clip stack is unbalanced";
          return false;
        }
        --clip_depth;
      }
    }
    if (clip_depth != 0) {
      error = "Canvas2D clip stack is unbalanced";
      return false;
    }
    clip_stack.clear();
    clip = {0, 0, static_cast<int>(width), static_cast<int>(height)};
    for (size_t index = 0; index < command_count; ++index) {
      const Canvas2dCommand &command = commands[index];
      switch (static_cast<Canvas2dOpcode>(command.opcode)) {
      case Canvas2dOpcode::Clear:
        std::fill(pixels.begin(), pixels.end(), 0);
        fillRect(command.x, command.y, command.width, command.height,
                 command.radius, command.rgba);
        break;
      case Canvas2dOpcode::FillRect:
        fillRect(command.x, command.y, command.width, command.height,
                 command.radius, command.rgba);
        break;
      case Canvas2dOpcode::StrokeRect:
        strokeRect(command);
        break;
      case Canvas2dOpcode::FillText:
        fillText(command, text + command.text_offset);
        break;
      case Canvas2dOpcode::PushClip: {
        clip_stack.push_back(clip);
        const int left = static_cast<int>(std::floor(command.x));
        const int top = static_cast<int>(std::floor(command.y));
        const int right = static_cast<int>(std::ceil(command.x + command.width));
        const int bottom =
            static_cast<int>(std::ceil(command.y + command.height));
        clip = {std::max(clip.left, left), std::max(clip.top, top),
                std::min(clip.right, right), std::min(clip.bottom, bottom)};
        break;
      }
      case Canvas2dOpcode::PopClip:
        clip = clip_stack.back();
        clip_stack.pop_back();
        break;
      }
    }
    return true;
  }

  float measure(const char *text, size_t text_size, float font_size) {
    if ((!text && text_size != 0) || !finite(font_size) || font_size <= 0)
      return 0;
    float width = 0;
    size_t offset = 0;
    while (offset < text_size) {
      const Glyph *value = glyph(
          nextCodepoint(reinterpret_cast<const uint8_t *>(text), text_size,
                        offset),
          font_size);
      if (value)
        width += value->advance;
    }
    return width;
  }

  resources::FontAssetService *fonts;
  FontFace primary;
  FontFace fallback;
  std::unordered_map<GlyphKey, Glyph, GlyphKeyHash> glyphs;
  std::vector<uint8_t> pixels;
  std::vector<ClipRect> clip_stack;
  std::string error;
  ClipRect clip = {0, 0, 0, 0};
  uint32_t width = 0;
  uint32_t height = 0;
  bool fonts_attempted = false;
};

Canvas2dRenderer::Canvas2dRenderer(uint32_t width, uint32_t height,
                                   resources::FontAssetService *fonts)
    : impl_(std::make_unique<Impl>(width, height, fonts)) {}

Canvas2dRenderer::~Canvas2dRenderer() = default;

bool Canvas2dRenderer::resize(uint32_t width, uint32_t height) {
  return impl_->resize(width, height);
}

bool Canvas2dRenderer::render(const Canvas2dCommand *commands,
                              size_t command_count, const uint8_t *text,
                              size_t text_size) {
  return impl_->render(commands, command_count, text, text_size);
}

float Canvas2dRenderer::measureText(const char *text, size_t text_size,
                                    float font_size) {
  return impl_->measure(text, text_size, font_size);
}

uint32_t Canvas2dRenderer::width() const { return impl_->width; }
uint32_t Canvas2dRenderer::height() const { return impl_->height; }
const uint8_t *Canvas2dRenderer::pixels() const { return impl_->pixels.data(); }
size_t Canvas2dRenderer::pixelBytes() const { return impl_->pixels.size(); }
const std::string &Canvas2dRenderer::lastError() const { return impl_->error; }

} // namespace oos::runtime
