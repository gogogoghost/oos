#include "app.h"
#include "oos_lvgl_backend.h"

#include <stdint.h>

static oos_lvgl_backend_t backend;
static uint8_t draw_buffer[240 * 8 * 2];
static lv_obj_t *status_panel;
static uint64_t previous_frame_us;

static bool submit_frame(void) {
  lv_timer_handler();
  oos_platform_graphics_vertex_t vertices[4];
  uint16_t indices[6];
  oos_platform_graphics_draw_command_t command;
  if (!oos_lvgl_backend_build_draw(&backend, vertices, indices, &command))
    return true;

  oos_platform_graphics_list_vertex_t vertex_list = {vertices, 4};
  app_list_u16_t index_list = {indices, 6};
  oos_platform_graphics_list_draw_command_t command_list = {&command, 1};
  oos_platform_graphics_error_code_t error;
  return oos_platform_graphics_submit(&vertex_list, &index_list, &command_list,
                                      0xff12181c, &error);
}

bool exports_oos_platform_lifecycle_init(
    exports_oos_platform_lifecycle_error_code_t *error) {
  lv_init();
  if (!oos_lvgl_backend_init(&backend, 1, draw_buffer, sizeof(draw_buffer))) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }

  lv_obj_t *screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x12181c), 0);
  lv_obj_set_style_pad_all(screen, 16, 0);
  lv_obj_set_style_pad_row(screen, 12, 0);
  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

  lv_obj_t *title = lv_label_create(screen);
  lv_label_set_text(title, "LVGL on OOS");
  lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);

  status_panel = lv_obj_create(screen);
  lv_obj_set_width(status_panel, LV_PCT(100));
  lv_obj_set_flex_grow(status_panel, 1);
  lv_obj_set_style_bg_color(status_panel, lv_color_hex(0x1f8a70), 0);
  lv_obj_set_style_border_width(status_panel, 0, 0);
  lv_obj_set_style_radius(status_panel, 6, 0);

  lv_obj_t *message = lv_label_create(status_panel);
  lv_label_set_text(message, "WIT mesh backend ready");
  lv_obj_set_style_text_color(message, lv_color_hex(0xffffff), 0);
  lv_obj_center(message);

  lv_obj_t *hint = lv_label_create(screen);
  lv_label_set_text(hint, "Press OK to change color");
  lv_obj_set_style_text_color(hint, lv_color_hex(0xb8c2c8), 0);
  return submit_frame();
}

void exports_oos_platform_lifecycle_event(
    exports_oos_platform_lifecycle_key_event_t *event) {
  if (event &&
      event->action == EXPORTS_OOS_PLATFORM_LIFECYCLE_KEY_ACTION_PRESSED &&
      event->code == 352 && status_panel) {
    static bool alternate;
    alternate = !alternate;
    lv_obj_set_style_bg_color(status_panel,
                              lv_color_hex(alternate ? 0xd65a31 : 0x1f8a70), 0);
  }
}

bool exports_oos_platform_lifecycle_frame(
    uint64_t monotonic_time_us, uint32_t *next_delay_ms,
    exports_oos_platform_lifecycle_error_code_t *error) {
  uint64_t elapsed =
      previous_frame_us == 0 ? 16 : monotonic_time_us - previous_frame_us;
  previous_frame_us = monotonic_time_us;
  if (elapsed > 1000)
    elapsed = 1000;
  lv_tick_inc((uint32_t)(elapsed / 1000));
  if (!submit_frame()) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  *next_delay_ms = 16;
  return true;
}

void exports_oos_platform_lifecycle_shutdown(void) {
  oos_lvgl_backend_destroy(&backend);
  lv_deinit();
  status_panel = NULL;
  previous_frame_us = 0;
}
