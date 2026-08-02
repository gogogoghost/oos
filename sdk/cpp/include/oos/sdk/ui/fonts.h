#pragma once

#include <cstdint>
#include <string>

#include <lvgl.h>

namespace oos::sdk::ui::fonts {

// Initializes the process-wide system font family after lv_init().
bool initialize();
void shutdown();

// Returns a system text font with device fallback and OOS icon glyphs.
// Supported sizes are 10, 12, 14, 20, and 36 pixels.
const lv_font_t *get(uint32_t size);
std::string regularPath();
const std::string &lastError();

} // namespace oos::sdk::ui::fonts
