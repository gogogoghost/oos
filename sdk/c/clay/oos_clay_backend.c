#include "oos_clay_backend.h"

#include <math.h>
#include <string.h>

_Static_assert(sizeof(oos_platform_canvas_canvas2d_command_t) == 44,
               "Canvas2D command ABI changed");

static uint8_t color_channel(float value) {
  if (value <= 0)
    return 0;
  if (value >= 255)
    return 255;
  return (uint8_t)lroundf(value);
}

static uint32_t pack_color(Clay_Color color) {
  return (uint32_t)color_channel(color.r) |
         ((uint32_t)color_channel(color.g) << 8) |
         ((uint32_t)color_channel(color.b) << 16) |
         ((uint32_t)color_channel(color.a) << 24);
}

static float maximum_radius(Clay_CornerRadius radius) {
  return fmaxf(fmaxf(radius.topLeft, radius.topRight),
               fmaxf(radius.bottomLeft, radius.bottomRight));
}

static bool append_command(oos_clay_backend_t *backend, size_t *count,
                           oos_platform_canvas_canvas2d_command_t command) {
  if (*count >= backend->command_capacity) {
    backend->error = OOS_CLAY_ERROR_CAPACITY;
    return false;
  }
  backend->commands[(*count)++] = command;
  return true;
}

static oos_platform_canvas_canvas2d_command_t
base_command(uint8_t opcode, Clay_BoundingBox bounds, uint32_t rgba) {
  oos_platform_canvas_canvas2d_command_t command;
  memset(&command, 0, sizeof(command));
  command.opcode = opcode;
  command.x = bounds.x;
  command.y = bounds.y;
  command.width = bounds.width;
  command.height = bounds.height;
  command.line_width = 1;
  command.font_size = 14;
  command.rgba = rgba;
  return command;
}

bool oos_clay_backend_attach(
    oos_clay_backend_t *backend, uint32_t canvas, uint32_t width,
    uint32_t height, oos_platform_canvas_canvas2d_command_t *command_storage,
    size_t command_capacity, uint8_t *text_storage, size_t text_capacity) {
  if (!backend || !canvas || !width || !height || !command_storage ||
      command_capacity < 2 || (!text_storage && text_capacity != 0))
    return false;
  memset(backend, 0, sizeof(*backend));
  backend->canvas = canvas;
  backend->width = width;
  backend->height = height;
  backend->commands = command_storage;
  backend->command_capacity = command_capacity;
  backend->text = text_storage;
  backend->text_capacity = text_capacity;
  return true;
}

bool oos_clay_backend_init(
    oos_clay_backend_t *backend, oos_platform_canvas_geometry_t geometry,
    oos_platform_canvas_canvas2d_command_t *command_storage,
    size_t command_capacity, uint8_t *text_storage, size_t text_capacity) {
  if (!backend || !geometry.width || !geometry.height)
    return false;
  uint32_t canvas = 0;
  oos_platform_canvas_error_code_t error;
  if (!oos_platform_canvas_create(OOS_PLATFORM_CANVAS_CONTEXT_KIND_CANVAS2D,
                                  &geometry, &canvas, &error)) {
    memset(backend, 0, sizeof(*backend));
    backend->error = OOS_CLAY_ERROR_PLATFORM;
    backend->platform_error = error;
    return false;
  }
  if (!oos_clay_backend_attach(backend, canvas, geometry.width, geometry.height,
                               command_storage, command_capacity, text_storage,
                               text_capacity)) {
    oos_platform_canvas_destroy(canvas, &error);
    return false;
  }
  backend->owns_canvas = true;
  return true;
}

bool oos_clay_backend_configure(oos_clay_backend_t *backend,
                                oos_platform_canvas_geometry_t geometry) {
  if (!backend || !backend->canvas || !geometry.width || !geometry.height) {
    if (backend)
      backend->error = OOS_CLAY_ERROR_INVALID_ARGUMENT;
    return false;
  }
  oos_platform_canvas_error_code_t error;
  if (!oos_platform_canvas_configure(backend->canvas, &geometry, &error)) {
    backend->error = OOS_CLAY_ERROR_PLATFORM;
    backend->platform_error = error;
    return false;
  }
  backend->width = geometry.width;
  backend->height = geometry.height;
  backend->error = OOS_CLAY_ERROR_NONE;
  return true;
}

static bool append_border(oos_clay_backend_t *backend, size_t *count,
                          Clay_RenderCommand *source) {
  const Clay_BorderRenderData *border = &source->renderData.border;
  const Clay_BorderWidth width = border->width;
  const uint32_t rgba = pack_color(border->color);
  if (width.left == width.right && width.left == width.top &&
      width.left == width.bottom && width.left != 0) {
    oos_platform_canvas_canvas2d_command_t command =
        base_command(OOS_PLATFORM_CANVAS_CANVAS2D_OPCODE_STROKE_RECT,
                     source->boundingBox, rgba);
    command.radius = maximum_radius(border->cornerRadius);
    command.line_width = width.left;
    return append_command(backend, count, command);
  }
  const Clay_BoundingBox bounds = source->boundingBox;
  const Clay_BoundingBox sides[4] = {
      {bounds.x, bounds.y, width.left, bounds.height},
      {bounds.x + bounds.width - width.right, bounds.y, width.right,
       bounds.height},
      {bounds.x, bounds.y, bounds.width, width.top},
      {bounds.x, bounds.y + bounds.height - width.bottom, bounds.width,
       width.bottom},
  };
  const uint16_t widths[4] = {width.left, width.right, width.top, width.bottom};
  for (size_t side = 0; side < 4; ++side) {
    if (widths[side] != 0 &&
        !append_command(
            backend, count,
            base_command(OOS_PLATFORM_CANVAS_CANVAS2D_OPCODE_FILL_RECT,
                         sides[side], rgba)))
      return false;
  }
  return true;
}

bool oos_clay_backend_render(oos_clay_backend_t *backend,
                             Clay_RenderCommandArray commands) {
  if (!backend || !backend->canvas || !backend->commands ||
      commands.length < 0 || commands.capacity < commands.length ||
      (commands.length != 0 && !commands.internalArray)) {
    if (backend)
      backend->error = OOS_CLAY_ERROR_INVALID_ARGUMENT;
    return false;
  }
  backend->error = OOS_CLAY_ERROR_NONE;
  size_t count = 0;
  size_t text_size = 0;
  size_t clip_depth = 0;
  Clay_BoundingBox surface = {0, 0, (float)backend->width,
                              (float)backend->height};
  if (!append_command(
          backend, &count,
          base_command(OOS_PLATFORM_CANVAS_CANVAS2D_OPCODE_CLEAR, surface, 0)))
    return false;

  for (int32_t index = 0; index < commands.length; ++index) {
    Clay_RenderCommand *source = &commands.internalArray[index];
    oos_platform_canvas_canvas2d_command_t command;
    switch (source->commandType) {
    case CLAY_RENDER_COMMAND_TYPE_NONE:
      continue;
    case CLAY_RENDER_COMMAND_TYPE_RECTANGLE:
      command = base_command(
          OOS_PLATFORM_CANVAS_CANVAS2D_OPCODE_FILL_RECT, source->boundingBox,
          pack_color(source->renderData.rectangle.backgroundColor));
      command.radius =
          maximum_radius(source->renderData.rectangle.cornerRadius);
      if (!append_command(backend, &count, command))
        return false;
      break;
    case CLAY_RENDER_COMMAND_TYPE_BORDER:
      if (!append_border(backend, &count, source))
        return false;
      break;
    case CLAY_RENDER_COMMAND_TYPE_TEXT: {
      Clay_TextRenderData *data = &source->renderData.text;
      if (data->stringContents.length < 0 ||
          (data->stringContents.length != 0 && !data->stringContents.chars) ||
          (size_t)data->stringContents.length >
              backend->text_capacity - text_size) {
        backend->error = OOS_CLAY_ERROR_CAPACITY;
        return false;
      }
      const size_t length = (size_t)data->stringContents.length;
      if (length)
        memcpy(backend->text + text_size, data->stringContents.chars, length);
      command = base_command(OOS_PLATFORM_CANVAS_CANVAS2D_OPCODE_FILL_TEXT,
                             source->boundingBox, pack_color(data->textColor));
      command.y += data->fontSize;
      command.font_size = data->fontSize;
      command.text_offset = (uint32_t)text_size;
      command.text_length = (uint32_t)length;
      text_size += length;
      if (!append_command(backend, &count, command))
        return false;
      break;
    }
    case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
      command = base_command(OOS_PLATFORM_CANVAS_CANVAS2D_OPCODE_PUSH_CLIP,
                             source->boundingBox, 0);
      if (++clip_depth > 64 || !append_command(backend, &count, command)) {
        backend->error = OOS_CLAY_ERROR_CAPACITY;
        return false;
      }
      break;
    case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
      if (clip_depth == 0) {
        backend->error = OOS_CLAY_ERROR_INVALID_ARGUMENT;
        return false;
      }
      --clip_depth;
      command = base_command(OOS_PLATFORM_CANVAS_CANVAS2D_OPCODE_POP_CLIP,
                             source->boundingBox, 0);
      if (!append_command(backend, &count, command))
        return false;
      break;
    case CLAY_RENDER_COMMAND_TYPE_IMAGE:
    case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
      backend->error = OOS_CLAY_ERROR_UNSUPPORTED_COMMAND;
      return false;
    }
  }
  if (clip_depth != 0) {
    backend->error = OOS_CLAY_ERROR_INVALID_ARGUMENT;
    return false;
  }
  oos_platform_canvas_list_canvas2d_command_t command_list = {backend->commands,
                                                              count};
  app_list_u8_t text_list = {backend->text, text_size};
  oos_platform_canvas_error_code_t error;
  if (!oos_platform_canvas_submit_2d(backend->canvas, &command_list, &text_list,
                                     &error)) {
    backend->error = OOS_CLAY_ERROR_PLATFORM;
    backend->platform_error = error;
    return false;
  }
  return true;
}

void oos_clay_backend_destroy(oos_clay_backend_t *backend) {
  if (!backend)
    return;
  if (backend->owns_canvas && backend->canvas) {
    oos_platform_canvas_error_code_t error;
    oos_platform_canvas_destroy(backend->canvas, &error);
  }
  memset(backend, 0, sizeof(*backend));
}

oos_clay_error_t
oos_clay_backend_last_error(const oos_clay_backend_t *backend,
                            oos_platform_canvas_error_code_t *platform_error) {
  if (!backend)
    return OOS_CLAY_ERROR_INVALID_ARGUMENT;
  if (platform_error)
    *platform_error = backend->platform_error;
  return backend->error;
}
