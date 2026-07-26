#include <stdint.h>
#include <unistd.h>

#include "cover_fixture.h"
#include "oos/nokia2780/display_control.h"

int main(void) {
  static uint16_t pixels[NOKIA_2780_COVER_WIDTH * NOKIA_2780_COVER_HEIGHT];
  nokia2780_make_secondary_frame(pixels);
  if (nokia2780_show_cover_rgb565(pixels, NOKIA_2780_COVER_WIDTH,
                                  NOKIA_2780_COVER_HEIGHT) != 0) {
    return 1;
  }
  for (;;)
    pause();
}
