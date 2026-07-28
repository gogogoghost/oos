#include "cover_fixture.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "oos/nokia2780/display_control.h"

namespace {

struct Glyph {
  char character;
  uint8_t rows[7];
};

constexpr Glyph kGlyphs[] = {
    {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}},
    {'a', {0x00, 0x00, 0x0e, 0x01, 0x0f, 0x11, 0x0f}},
    {'c', {0x00, 0x00, 0x0e, 0x10, 0x10, 0x10, 0x0e}},
    {'d', {0x00, 0x01, 0x01, 0x0f, 0x11, 0x11, 0x0f}},
    {'e', {0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e}},
    {'n', {0x00, 0x00, 0x1e, 0x11, 0x11, 0x11, 0x11}},
    {'o', {0x00, 0x00, 0x0e, 0x11, 0x11, 0x11, 0x0e}},
    {'r', {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}},
    {'s', {0x00, 0x00, 0x0f, 0x10, 0x0e, 0x01, 0x1e}},
    {'y', {0x00, 0x00, 0x11, 0x11, 0x0f, 0x01, 0x0e}},
};

const uint8_t *glyphFor(char character) {
  for (const auto &glyph : kGlyphs) {
    if (glyph.character == character)
      return glyph.rows;
  }
  return nullptr;
}

void drawText(uint16_t *pixels, uint32_t y, const char *text) {
  constexpr uint32_t kScale = 2;
  constexpr uint32_t kAdvance = 6 * kScale;
  const size_t length = std::strlen(text);
  const uint32_t text_width =
      length ? static_cast<uint32_t>(length * kAdvance - kScale) : 0;
  uint32_t x = NOKIA_2780_COVER_WIDTH > text_width
                   ? (NOKIA_2780_COVER_WIDTH - text_width) / 2
                   : 0;

  for (size_t index = 0; index < length; ++index) {
    const uint8_t *rows = glyphFor(text[index]);
    if (!rows) {
      x += kAdvance;
      continue;
    }
    for (uint32_t row = 0; row < 7; ++row) {
      for (uint32_t column = 0; column < 5; ++column) {
        if (!(rows[row] & (1u << (4 - column))))
          continue;
        for (uint32_t dy = 0; dy < kScale; ++dy) {
          for (uint32_t dx = 0; dx < kScale; ++dx) {
            pixels[(y + row * kScale + dy) * NOKIA_2780_COVER_WIDTH + x +
                   column * kScale + dx] = 0xffff;
          }
        }
      }
    }
    x += kAdvance;
  }
}

} // namespace

extern "C" void nokia2780_make_secondary_frame(uint16_t *pixels) {
  constexpr uint16_t kBackground = 0x01c4;
  for (size_t i = 0; i < NOKIA_2780_COVER_WIDTH * NOKIA_2780_COVER_HEIGHT;
       ++i) {
    pixels[i] = kBackground;
  }
  drawText(pixels, 54, "Secondary");
  drawText(pixels, 82, "screen");
}
