#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  NOKIA_2780_COVER_WIDTH = 128,
  NOKIA_2780_COVER_HEIGHT = 160,
};

// Makes the primary panel the only active display. The caller still owns HWC
// frame submission and may set the final primary backlight level afterwards.
int nokia2780_prepare_primary(void);

// Makes the cover panel the only active display and presents one RGB565 frame.
// The frame is copied to every fb1 virtual page before the backlight is
// enabled.
int nokia2780_show_cover_rgb565(const uint16_t *pixels, uint32_t width,
                                uint32_t height);

// Variants for a long-lived process that owns the primary HWC client. The
// caller must power the primary display off before presenting the cover and
// power it on only after nokia2780_hide_cover() returns.
int nokia2780_show_cover_rgb565_after_primary_off(const uint16_t *pixels,
                                                  uint32_t width,
                                                  uint32_t height);
int nokia2780_hide_cover(void);

#ifdef __cplusplus
}
#endif
