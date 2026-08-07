#include "oos_clay_backend.h"

void oos_clay_backend_compile_test(Clay_RenderCommandArray commands) {
  oos_clay_backend_t backend;
  oos_platform_canvas_canvas2d_command_t command_storage[32];
  uint8_t text_storage[256];
  oos_platform_canvas_geometry_t geometry = {0, 0, 240, 320, 0, true};
  if (oos_clay_backend_init(&backend, geometry, command_storage, 32,
                            text_storage, sizeof(text_storage))) {
    oos_clay_backend_render(&backend, commands);
    oos_clay_backend_destroy(&backend);
  }
}
