#include "oos_lvgl_backend.h"

#include <stdlib.h>
#include <string.h>

static void flush(lv_display_t *display, const lv_area_t *area,
                  uint8_t *pixels) {
  oos_lvgl_backend_t *backend = lv_display_get_user_data(display);
  const uint32_t width = (uint32_t)(area->x2 - area->x1 + 1);
  const uint32_t height = (uint32_t)(area->y2 - area->y1 + 1);
  const uint32_t bytes_per_pixel = backend->transparent ? 4 : 2;
  if (backend->transparent) {
    const size_t count = (size_t)width * height;
    for (size_t index = 0; index < count; ++index) {
      const lv_color32_t source = ((const lv_color32_t *)pixels)[index];
      const uint32_t alpha = source.alpha;
      pixels[index * 4] = (uint8_t)((source.red * alpha + 127) / 255);
      pixels[index * 4 + 1] = (uint8_t)((source.green * alpha + 127) / 255);
      pixels[index * 4 + 2] = (uint8_t)((source.blue * alpha + 127) / 255);
      pixels[index * 4 + 3] = source.alpha;
    }
  }
  oos_platform_graphics_point_t position = {(uint32_t)area->x1,
                                            (uint32_t)area->y1};
  oos_platform_graphics_size_t dimensions = {width, height};
  app_list_u8_t bytes = {pixels, (size_t)width * height * bytes_per_pixel};
  oos_platform_graphics_error_code_t error;
  if (!oos_platform_graphics_texture_set(
          backend->texture,
          backend->transparent ? OOS_PLATFORM_GRAPHICS_TEXTURE_FORMAT_RGBA8888
                               : OOS_PLATFORM_GRAPHICS_TEXTURE_FORMAT_RGB565,
          &position, &dimensions, width * bytes_per_pixel, 0, &bytes, &error))
    backend->dirty = false;
  else
    backend->dirty = true;
  lv_display_flush_ready(display);
}

static bool init_backend(oos_lvgl_backend_t *backend, uint32_t texture,
                         void *draw_buffer, size_t draw_buffer_bytes,
                         bool transparent) {
  if (!backend || !texture || !draw_buffer || draw_buffer_bytes < 4)
    return false;
  memset(backend, 0, sizeof(*backend));
  oos_platform_graphics_size_t size;
  oos_platform_graphics_surface_size(&size);
  const uint32_t bytes_per_pixel = transparent ? 4 : 2;
  if (!size.width || !size.height ||
      draw_buffer_bytes < (size_t)size.width * bytes_per_pixel)
    return false;
  uint8_t *initial = calloc((size_t)size.width * size.height, bytes_per_pixel);
  if (!initial)
    return false;
  oos_platform_graphics_point_t origin = {0, 0};
  app_list_u8_t bytes = {initial,
                         (size_t)size.width * size.height * bytes_per_pixel};
  oos_platform_graphics_error_code_t error;
  const bool uploaded = oos_platform_graphics_texture_set(
      texture,
      transparent ? OOS_PLATFORM_GRAPHICS_TEXTURE_FORMAT_RGBA8888
                  : OOS_PLATFORM_GRAPHICS_TEXTURE_FORMAT_RGB565,
      &origin, &size, size.width * bytes_per_pixel,
      OOS_PLATFORM_GRAPHICS_TEXTURE_FLAGS_REPLACE, &bytes, &error);
  free(initial);
  if (!uploaded)
    return false;
  backend->texture = texture;
  backend->width = size.width;
  backend->height = size.height;
  backend->draw_buffer = draw_buffer;
  backend->draw_buffer_bytes = draw_buffer_bytes;
  backend->transparent = transparent;
  backend->display =
      lv_display_create((int32_t)size.width, (int32_t)size.height);
  if (!backend->display) {
    oos_platform_graphics_texture_free(texture, &error);
    return false;
  }
  lv_display_set_user_data(backend->display, backend);
  lv_display_set_color_format(backend->display, transparent
                                                    ? LV_COLOR_FORMAT_ARGB8888
                                                    : LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(backend->display, draw_buffer, NULL, draw_buffer_bytes,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(backend->display, flush);
  if (transparent) {
    lv_obj_t *screen = lv_display_get_screen_active(backend->display);
    lv_obj_set_style_bg_opa(screen, LV_OPA_TRANSP, 0);
  }
  return true;
}

bool oos_lvgl_backend_init(oos_lvgl_backend_t *backend, uint32_t texture,
                           void *draw_buffer, size_t draw_buffer_bytes) {
  return init_backend(backend, texture, draw_buffer, draw_buffer_bytes, false);
}

bool oos_lvgl_backend_init_overlay(oos_lvgl_backend_t *backend,
                                   uint32_t texture, void *draw_buffer,
                                   size_t draw_buffer_bytes) {
  return init_backend(backend, texture, draw_buffer, draw_buffer_bytes, true);
}

bool oos_lvgl_backend_resize(oos_lvgl_backend_t *backend, uint32_t width,
                             uint32_t height) {
  if (!backend || !backend->display || !width || !height)
    return false;
  const uint32_t bytes_per_pixel = backend->transparent ? 4 : 2;
  if (backend->draw_buffer_bytes < (size_t)width * bytes_per_pixel)
    return false;
  uint8_t *initial = calloc((size_t)width * height, bytes_per_pixel);
  if (!initial)
    return false;
  oos_platform_graphics_point_t origin = {0, 0};
  oos_platform_graphics_size_t size = {width, height};
  app_list_u8_t bytes = {initial, (size_t)width * height * bytes_per_pixel};
  oos_platform_graphics_error_code_t error;
  const bool uploaded = oos_platform_graphics_texture_set(
      backend->texture,
      backend->transparent ? OOS_PLATFORM_GRAPHICS_TEXTURE_FORMAT_RGBA8888
                           : OOS_PLATFORM_GRAPHICS_TEXTURE_FORMAT_RGB565,
      &origin, &size, width * bytes_per_pixel,
      OOS_PLATFORM_GRAPHICS_TEXTURE_FLAGS_REPLACE, &bytes, &error);
  free(initial);
  if (!uploaded)
    return false;
  backend->width = width;
  backend->height = height;
  backend->dirty = true;
  lv_display_set_resolution(backend->display, (int32_t)width, (int32_t)height);
  lv_obj_invalidate(lv_display_get_screen_active(backend->display));
  return true;
}

void oos_lvgl_backend_destroy(oos_lvgl_backend_t *backend) {
  if (!backend)
    return;
  if (backend->display)
    lv_display_delete(backend->display);
  if (backend->texture) {
    oos_platform_graphics_error_code_t error;
    oos_platform_graphics_texture_free(backend->texture, &error);
  }
  memset(backend, 0, sizeof(*backend));
}

bool oos_lvgl_backend_build_draw(
    oos_lvgl_backend_t *backend, oos_platform_graphics_vertex_t vertices[4],
    uint16_t indices[6], oos_platform_graphics_draw_command_t *command) {
  if (!backend || !backend->display || !backend->dirty || !vertices ||
      !indices || !command)
    return false;
  const float width = (float)backend->width;
  const float height = (float)backend->height;
  const oos_platform_graphics_vertex_t quad[4] = {
      {0, 0, 0, 0, 255, 255, 255, 255},
      {width, 0, 1, 0, 255, 255, 255, 255},
      {width, height, 1, 1, 255, 255, 255, 255},
      {0, height, 0, 1, 255, 255, 255, 255}};
  const uint16_t quad_indices[6] = {0, 1, 2, 0, 2, 3};
  memcpy(vertices, quad, sizeof(quad));
  memcpy(indices, quad_indices, sizeof(quad_indices));
  *command = (oos_platform_graphics_draw_command_t){
      0, 6, backend->texture, 0, 0, width, height};
  backend->dirty = false;
  return true;
}
