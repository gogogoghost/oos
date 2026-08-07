#define CLAY_IMPLEMENTATION
#include "oos_clay_backend.h"

#include <stdlib.h>

static oos_clay_backend_t backend;
static oos_platform_canvas_canvas2d_command_t commands[64];
static uint8_t text[512];
static void *clay_memory;
static bool alternate;

static void handle_clay_error(Clay_ErrorData error) { (void)error; }

static Clay_Dimensions measure_text(Clay_StringSlice value,
                                    Clay_TextElementConfig *config,
                                    void *user_data) {
  (void)user_data;
  return (Clay_Dimensions){
      (float)value.length * (float)config->fontSize * 0.56f,
      (float)config->fontSize * 1.2f,
  };
}

static bool render_layout(void) {
  const Clay_Color accent = alternate ? (Clay_Color){214, 90, 49, 255}
                                      : (Clay_Color){31, 138, 112, 255};
  Clay_BeginLayout();
  CLAY({
      .id = CLAY_ID("Root"),
      .layout =
          {
              .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
              .padding = CLAY_PADDING_ALL(16),
              .childGap = 12,
              .layoutDirection = CLAY_TOP_TO_BOTTOM,
          },
      .backgroundColor = {18, 24, 28, 255},
  }) {
    CLAY_TEXT(
        CLAY_STRING("Clay on OOS"),
        CLAY_TEXT_CONFIG({.fontSize = 20, .textColor = {255, 255, 255, 255}}));
    CLAY({
        .id = CLAY_ID("Status"),
        .layout =
            {
                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                .padding = CLAY_PADDING_ALL(16),
                .childAlignment = {.x = CLAY_ALIGN_X_CENTER,
                                   .y = CLAY_ALIGN_Y_CENTER},
            },
        .backgroundColor = accent,
        .cornerRadius = CLAY_CORNER_RADIUS(6),
    }) {
      CLAY_TEXT(CLAY_STRING("Canvas2D batch ready"),
                CLAY_TEXT_CONFIG(
                    {.fontSize = 16, .textColor = {255, 255, 255, 255}}));
    }
    CLAY_TEXT(
        CLAY_STRING("Press OK to change color"),
        CLAY_TEXT_CONFIG({.fontSize = 13, .textColor = {184, 194, 200, 255}}));
  }
  return oos_clay_backend_render(&backend, Clay_EndLayout());
}

bool exports_oos_platform_lifecycle_init(
    exports_oos_platform_lifecycle_error_code_t *error) {
  Clay_SetMaxElementCount(128);
  Clay_SetMaxMeasureTextCacheWordCount(256);
  const uint32_t memory_size = Clay_MinMemorySize();
  clay_memory = malloc(memory_size);
  if (!clay_memory) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_LIMIT_EXCEEDED;
    return false;
  }
  Clay_Arena arena =
      Clay_CreateArenaWithCapacityAndMemory(memory_size, clay_memory);
  Clay_Initialize(arena, (Clay_Dimensions){240, 298},
                  (Clay_ErrorHandler){handle_clay_error});
  Clay_SetMeasureTextFunction(measure_text, NULL);

  oos_platform_canvas_geometry_t geometry = {0, 0, 240, 298, 0, true};
  if (!oos_clay_backend_init(&backend, geometry, commands,
                             sizeof(commands) / sizeof(commands[0]), text,
                             sizeof(text)) ||
      !render_layout()) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  return true;
}

void exports_oos_platform_lifecycle_event(
    exports_oos_platform_lifecycle_key_event_t *event) {
  if (event &&
      event->action == EXPORTS_OOS_PLATFORM_LIFECYCLE_KEY_ACTION_PRESSED &&
      event->code == 352)
    alternate = !alternate;
}

bool exports_oos_platform_lifecycle_frame(
    uint64_t monotonic_time_us, uint32_t *next_delay_ms,
    exports_oos_platform_lifecycle_error_code_t *error) {
  (void)monotonic_time_us;
  if (!render_layout()) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  *next_delay_ms = 1000;
  return true;
}

void exports_oos_platform_lifecycle_shutdown(void) {
  oos_clay_backend_destroy(&backend);
  free(clay_memory);
  clay_memory = NULL;
  alternate = false;
}
