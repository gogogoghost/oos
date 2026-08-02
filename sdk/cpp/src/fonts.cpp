#include "oos/sdk/ui/fonts.h"

#include "oos/sdk/ui/icon_fonts.h"

#include <array>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <unistd.h>

extern "C" {
const lv_font_t *oos_lvgl_default_font = nullptr;
}

namespace oos::sdk::ui::fonts {
namespace {

struct FontSize {
  uint32_t pixels;
  size_t primary_cache;
  size_t fallback_cache;
  const lv_font_t *icons;
  lv_font_t *primary = nullptr;
  lv_font_t *fallback = nullptr;
};

std::array<FontSize, 4> g_fonts = {{{12, 20, 12, &oos_icon_font_12},
                                    {14, 20, 12, &oos_icon_font_14},
                                    {20, 16, 10, &oos_icon_font_20},
                                    {36, 12, 8, &oos_icon_font_20}}};
std::string g_error;
bool g_initialized = false;

std::string firstReadable(const char *environment,
                          std::initializer_list<const char *> candidates) {
  const char *configured = std::getenv(environment);
  if (configured && configured[0] == '/' && access(configured, R_OK) == 0)
    return configured;
  for (const char *candidate : candidates) {
    if (access(candidate, R_OK) == 0)
      return candidate;
  }
  return {};
}

std::string lvglPath(const std::string &path) { return "A:" + path; }

lv_font_t *create(const std::string &path, uint32_t pixels, size_t cache_size) {
  const std::string source = lvglPath(path);
  return lv_tiny_ttf_create_file_ex(source.c_str(), pixels,
                                    LV_FONT_KERNING_NORMAL, cache_size);
}

void destroyFonts() {
  for (FontSize &font : g_fonts) {
    if (font.primary)
      lv_tiny_ttf_destroy(font.primary);
    if (font.fallback)
      lv_tiny_ttf_destroy(font.fallback);
    font.primary = nullptr;
    font.fallback = nullptr;
  }
}

} // namespace

std::string regularPath() {
  return firstReadable(
      "OOS_UI_FONT_REGULAR",
      {"/system/fonts/Roboto-Regular.ttf",
       "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
       "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf"});
}

bool initialize() {
  if (g_initialized)
    return true;
  g_error.clear();
  const std::string primary = regularPath();
  const std::string fallback =
      firstReadable("OOS_UI_FONT_FALLBACK",
                    {"/system/fonts/DroidSansFallback.ttf",
                     "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                     "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"});
  if (primary.empty()) {
    g_error = "no readable system UI font was found";
    return false;
  }

  for (FontSize &font : g_fonts) {
    font.primary = create(primary, font.pixels, font.primary_cache);
    if (!font.primary) {
      g_error = "load system UI font failed: " + primary;
      destroyFonts();
      return false;
    }
    if (!fallback.empty() && fallback != primary) {
      font.fallback = create(fallback, font.pixels, font.fallback_cache);
      if (!font.fallback) {
        g_error = "load system fallback font failed: " + fallback;
        destroyFonts();
        return false;
      }
    }
    font.primary->fallback = font.fallback ? font.fallback : font.icons;
    if (font.fallback)
      font.fallback->fallback = font.icons;
  }
  oos_lvgl_default_font = g_fonts[1].primary;
  g_initialized = true;
  return true;
}

void shutdown() {
  if (!g_initialized)
    return;
  oos_lvgl_default_font = nullptr;
  destroyFonts();
  g_initialized = false;
}

const lv_font_t *get(uint32_t size) {
  for (const FontSize &font : g_fonts) {
    if (font.pixels == size && font.primary)
      return font.primary;
  }
  return oos_lvgl_default_font;
}

const std::string &lastError() { return g_error; }

} // namespace oos::sdk::ui::fonts
