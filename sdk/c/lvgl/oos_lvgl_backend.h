#pragma once

#include "app.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  lv_display_t *display;
  void *draw_buffer;
  size_t draw_buffer_bytes;
  uint32_t texture;
  uint32_t width;
  uint32_t height;
  bool transparent;
  bool dirty;
} oos_lvgl_backend_t;

// The application owns `draw_buffer`. Two to eight RGB565 rows are normally
// sufficient for keypad UIs. The backend uploads only LVGL's invalidated areas.
bool oos_lvgl_backend_init(oos_lvgl_backend_t *backend, uint32_t texture,
                           void *draw_buffer, size_t draw_buffer_bytes);
// Alpha mode renders LVGL as ARGB8888, converts dirty rows in place to OOS
// premultiplied RGBA8888, and leaves transparent pixels compositable over a
// retained game framebuffer.
bool oos_lvgl_backend_init_overlay(oos_lvgl_backend_t *backend,
                                   uint32_t texture, void *draw_buffer,
                                   size_t draw_buffer_bytes);
// Recreates the retained texture and updates LVGL's logical resolution.
bool oos_lvgl_backend_resize(oos_lvgl_backend_t *backend, uint32_t width,
                             uint32_t height);
void oos_lvgl_backend_destroy(oos_lvgl_backend_t *backend);

// Produces one textured quad without submitting it. This lets an application
// combine LVGL, a game framebuffer, and other layers in one graphics.submit.
bool oos_lvgl_backend_build_draw(oos_lvgl_backend_t *backend,
                                 oos_platform_graphics_vertex_t vertices[4],
                                 uint16_t indices[6],
                                 oos_platform_graphics_draw_command_t *command);
