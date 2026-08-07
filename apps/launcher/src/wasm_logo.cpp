#include "oos/apps/launcher/logo.h"

#include <cstdint>

extern "C" {
extern unsigned char oos_launcher_logo_pixels[];
extern unsigned int oos_launcher_logo_pixels_len;
}

namespace oos::apps::launcher {

const lv_image_dsc_t kLogoImage = [] {
  lv_image_dsc_t image{};
  image.header.magic = LV_IMAGE_HEADER_MAGIC;
  image.header.cf = LV_COLOR_FORMAT_ARGB8888;
  image.header.w = 32;
  image.header.h = 32;
  image.header.stride = 32 * 4;
  image.data_size = oos_launcher_logo_pixels_len;
  image.data = oos_launcher_logo_pixels;
  return image;
}();

} // namespace oos::apps::launcher
