#include "oos/sdk/ui/fonts.h"

extern "C" {
const lv_font_t *oos_lvgl_default_font = nullptr;
}

namespace oos::sdk::ui::fonts {
namespace {
std::string g_error;
}

bool initialize() {
  oos_lvgl_default_font = &lv_font_montserrat_14;
  return true;
}

void shutdown() { oos_lvgl_default_font = nullptr; }

const lv_font_t *get(uint32_t size) {
  switch (size) {
  case 12:
    return &lv_font_montserrat_12;
  case 14:
    return &lv_font_montserrat_14;
  case 20:
    return &lv_font_montserrat_20;
  case 36:
    return &lv_font_montserrat_36;
  default:
    return oos_lvgl_default_font;
  }
}

std::string regularPath() { return {}; }
const std::string &lastError() { return g_error; }

} // namespace oos::sdk::ui::fonts
