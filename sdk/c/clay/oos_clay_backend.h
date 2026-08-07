#pragma once

#include "app.h"
#include "clay.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  OOS_CLAY_ERROR_NONE = 0,
  OOS_CLAY_ERROR_INVALID_ARGUMENT,
  OOS_CLAY_ERROR_CAPACITY,
  OOS_CLAY_ERROR_UNSUPPORTED_COMMAND,
  OOS_CLAY_ERROR_PLATFORM,
} oos_clay_error_t;

// Scratch storage belongs to the application and is reused every frame. A
// complete Clay render list crosses the host boundary in one Canvas2D submit.
typedef struct {
  uint32_t canvas;
  uint32_t width;
  uint32_t height;
  oos_platform_canvas_canvas2d_command_t *commands;
  size_t command_capacity;
  uint8_t *text;
  size_t text_capacity;
  oos_clay_error_t error;
  oos_platform_canvas_error_code_t platform_error;
  bool owns_canvas;
} oos_clay_backend_t;

bool oos_clay_backend_init(
    oos_clay_backend_t *backend, oos_platform_canvas_geometry_t geometry,
    oos_platform_canvas_canvas2d_command_t *command_storage,
    size_t command_capacity, uint8_t *text_storage, size_t text_capacity);

// Attaches Clay to a Canvas2D canvas supplied by another UI tree, such as a
// Solid <canvas> node. The attached canvas remains owned by its creator.
bool oos_clay_backend_attach(
    oos_clay_backend_t *backend, uint32_t canvas, uint32_t width,
    uint32_t height, oos_platform_canvas_canvas2d_command_t *command_storage,
    size_t command_capacity, uint8_t *text_storage, size_t text_capacity);

bool oos_clay_backend_configure(oos_clay_backend_t *backend,
                                oos_platform_canvas_geometry_t geometry);
bool oos_clay_backend_render(oos_clay_backend_t *backend,
                             Clay_RenderCommandArray commands);
void oos_clay_backend_destroy(oos_clay_backend_t *backend);

oos_clay_error_t
oos_clay_backend_last_error(const oos_clay_backend_t *backend,
                            oos_platform_canvas_error_code_t *platform_error);
