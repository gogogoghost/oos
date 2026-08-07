#include "oos/runtime/application_scene.h"

#include "oos/resources/font_assets.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oos::runtime {
namespace {

constexpr uint32_t kMaximumCanvases = 64;
constexpr uint64_t kMaximumScenePixels = 8 * 1024 * 1024;

uint32_t nextTargetTexture() {
  static std::atomic<uint32_t> next{0x10000000u};
  uint32_t value = next.fetch_add(1, std::memory_order_relaxed);
  if (value == 0)
    value = next.fetch_add(1, std::memory_order_relaxed);
  return value;
}

bool validGeometry(const CanvasGeometry &geometry, uint32_t width,
                   uint32_t height) {
  return geometry.x >= 0 && geometry.y >= 0 && geometry.width != 0 &&
         geometry.height != 0 &&
         static_cast<uint64_t>(geometry.x) + geometry.width <= width &&
         static_cast<uint64_t>(geometry.y) + geometry.height <= height;
}

} // namespace

class ApplicationScene::Impl {
public:
  struct Canvas {
    CanvasGeometry geometry;
    CanvasContextKind context = CanvasContextKind::None;
    std::unordered_map<uint32_t, uint32_t> textures;
    std::unordered_map<uint32_t, uint32_t> buffers;
    std::unordered_map<uint32_t, uint32_t> shaders;
    std::unordered_map<uint32_t, uint32_t> programs;
    std::unique_ptr<Canvas2dRenderer> renderer;
    uint32_t canvas_texture = 0;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    std::vector<OosGfxVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<OosGfxDrawCommand> commands;
    uint32_t clear_rgba = 0;
    bool texture_live = false;
    bool pixels_dirty = false;
    bool has_frame = false;
  };

  Impl(GraphicsHost &target, std::string font_directory)
      : target(target), fonts(std::move(font_directory)),
        text_metrics(1, 1, &fonts) {
    Canvas root;
    root.geometry = {0, 0, target.width(), target.height(), 0, true};
    root.render_width = target.width();
    root.render_height = target.height();
    root.clear_rgba = 0xff000000u;
    canvases.emplace(0, std::move(root));
  }

  ~Impl() { reset(); }

  Canvas *find(uint32_t handle) {
    const auto found = canvases.find(handle);
    return found == canvases.end() ? nullptr : &found->second;
  }

  bool requireContext(Canvas &canvas, CanvasContextKind context) {
    if (canvas.context == CanvasContextKind::None)
      canvas.context = context;
    if (canvas.context != context) {
      error = "a canvas cannot switch context type after getContext";
      return false;
    }
    return true;
  }

  uint64_t scenePixelsWith(uint32_t replacing, uint32_t width,
                           uint32_t height) const {
    uint64_t pixels = 0;
    for (const auto &entry : canvases) {
      if (entry.first == replacing)
        continue;
      pixels += static_cast<uint64_t>(entry.second.geometry.width) *
                entry.second.geometry.height;
    }
    return pixels + static_cast<uint64_t>(width) * height;
  }

  uint32_t create(const CanvasGeometry &geometry, CanvasContextKind context) {
    error.clear();
    if (canvases.size() >= kMaximumCanvases ||
        !validGeometry(geometry, target.width(), target.height()) ||
        context == CanvasContextKind::None ||
        scenePixelsWith(UINT32_MAX, geometry.width, geometry.height) >
            kMaximumScenePixels) {
      error = "canvas geometry, context, or scene limit is invalid";
      return 0;
    }
    uint32_t handle = next_canvas++;
    if (handle == 0)
      handle = next_canvas++;
    Canvas canvas;
    canvas.geometry = geometry;
    canvas.context = context;
    canvas.render_width = geometry.width;
    canvas.render_height = geometry.height;
    if (context == CanvasContextKind::Canvas2d) {
      canvas.renderer = std::make_unique<Canvas2dRenderer>(
          geometry.width, geometry.height, &fonts);
      if (!canvas.renderer || canvas.renderer->pixelBytes() == 0) {
        error = "allocate Canvas2D renderer failed";
        return 0;
      }
      canvas.canvas_texture = nextTargetTexture();
    } else if (context == CanvasContextKind::Gles2) {
      canvas.canvas_texture = nextTargetTexture();
    }
    canvases.emplace(handle, std::move(canvas));
    dirty = true;
    return handle;
  }

  bool configure(uint32_t handle, const CanvasGeometry &geometry) {
    Canvas *canvas = find(handle);
    if (!canvas || handle == 0 ||
        !validGeometry(geometry, target.width(), target.height()) ||
        scenePixelsWith(handle, geometry.width, geometry.height) >
            kMaximumScenePixels) {
      error = "canvas handle or geometry is invalid";
      return false;
    }
    const bool resized = canvas->render_width != geometry.width ||
                         canvas->render_height != geometry.height;
    if (resized && canvas->texture_live) {
      if (!target.freeTexture(canvas->canvas_texture)) {
        error = "release resized canvas texture failed";
        return false;
      }
      canvas->texture_live = false;
    }
    if (canvas->context == CanvasContextKind::Canvas2d && resized) {
      if (!canvas->renderer->resize(geometry.width, geometry.height)) {
        error = canvas->renderer->lastError();
        return false;
      }
      canvas->pixels_dirty = true;
    } else if (canvas->context == CanvasContextKind::Mesh2d && resized) {
      canvas->vertices.clear();
      canvas->indices.clear();
      canvas->commands.clear();
      canvas->has_frame = false;
    } else if (canvas->context == CanvasContextKind::Gles2 && resized) {
      canvas->has_frame = false;
    }
    canvas->render_width = geometry.width;
    canvas->render_height = geometry.height;
    canvas->geometry = geometry;
    dirty = true;
    return true;
  }

  bool place(uint32_t handle, const CanvasGeometry &geometry) {
    Canvas *canvas = find(handle);
    if (!canvas || handle == 0 ||
        !validGeometry(geometry, target.width(), target.height()) ||
        scenePixelsWith(handle, geometry.width, geometry.height) >
            kMaximumScenePixels) {
      error = "canvas handle or layout geometry is invalid";
      return false;
    }
    canvas->geometry = geometry;
    dirty = true;
    return true;
  }

  void release(Canvas &canvas) {
    for (const auto &program : canvas.programs)
      target.freeGlesProgram(program.second);
    for (const auto &shader : canvas.shaders)
      target.freeGlesShader(shader.second);
    for (const auto &buffer : canvas.buffers)
      target.freeGlesBuffer(buffer.second);
    for (const auto &texture : canvas.textures)
      target.freeTexture(texture.second);
    canvas.programs.clear();
    canvas.shaders.clear();
    canvas.buffers.clear();
    canvas.textures.clear();
    if (canvas.texture_live)
      target.freeTexture(canvas.canvas_texture);
    canvas.texture_live = false;
  }

  bool destroy(uint32_t handle) {
    const auto found = canvases.find(handle);
    if (found == canvases.end() || handle == 0) {
      error = "canvas handle is invalid or names the root canvas";
      return false;
    }
    release(found->second);
    canvases.erase(found);
    dirty = true;
    return true;
  }

  bool submit2d(uint32_t handle, const Canvas2dCommand *commands,
                size_t command_count, const uint8_t *text, size_t text_size) {
    Canvas *canvas = find(handle);
    if (!canvas || !requireContext(*canvas, CanvasContextKind::Canvas2d)) {
      if (!canvas)
        error = "Canvas2D handle is invalid";
      return false;
    }
    if (!canvas->renderer) {
      canvas->renderer = std::make_unique<Canvas2dRenderer>(
          canvas->geometry.width, canvas->geometry.height, &fonts);
      canvas->canvas_texture = nextTargetTexture();
    }
    if (!canvas->renderer->render(commands, command_count, text, text_size)) {
      error = canvas->renderer->lastError();
      return false;
    }
    canvas->pixels_dirty = true;
    canvas->has_frame = true;
    dirty = true;
    return true;
  }

  bool mapTexture(Canvas &canvas, uint32_t guest, uint32_t &target_handle,
                  bool create) {
    const auto found = canvas.textures.find(guest);
    if (found != canvas.textures.end()) {
      target_handle = found->second;
      return true;
    }
    if (!create || guest == 0)
      return false;
    target_handle = nextTargetTexture();
    canvas.textures.emplace(guest, target_handle);
    return true;
  }

  bool textureSet(uint32_t canvas_handle, uint32_t texture, uint32_t format,
                  uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                  uint32_t row_stride, uint32_t flags, const uint8_t *pixels,
                  size_t pixel_bytes) {
    Canvas *canvas = find(canvas_handle);
    if (!canvas || !requireContext(*canvas, CanvasContextKind::Mesh2d)) {
      if (!canvas)
        error = "mesh canvas handle is invalid";
      return false;
    }
    uint32_t target_handle = 0;
    const bool existing = mapTexture(*canvas, texture, target_handle, false);
    if (!existing && (x != 0 || y != 0)) {
      error = "a new canvas texture requires a full replacement upload";
      return false;
    }
    if (!existing)
      mapTexture(*canvas, texture, target_handle, true);
    const uint32_t target_flags = existing ? flags : flags | OOS_TEXTURE_REPLACE;
    if (target.setTexture(target_handle, format, x, y, width, height,
                          row_stride, target_flags, pixels, pixel_bytes))
      return true;
    if (!existing)
      canvas->textures.erase(texture);
    error = "canvas texture upload failed";
    return false;
  }

  bool textureFree(uint32_t canvas_handle, uint32_t texture) {
    Canvas *canvas = find(canvas_handle);
    if (!canvas || !requireContext(*canvas, CanvasContextKind::Mesh2d)) {
      if (!canvas)
      error = "mesh canvas handle is invalid";
      return false;
    }
    const auto found = canvas->textures.find(texture);
    if (found == canvas->textures.end())
      return true;
    if (!target.freeTexture(found->second)) {
      error = "free canvas texture failed";
      return false;
    }
    canvas->textures.erase(found);
    return true;
  }

  bool submitMesh(uint32_t canvas_handle, const OosGfxVertex *vertices,
                  size_t vertex_count, const uint16_t *indices,
                  size_t index_count, const OosGfxDrawCommand *commands,
                  size_t command_count, uint32_t clear_rgba) {
    Canvas *canvas = find(canvas_handle);
    if (!canvas || !requireContext(*canvas, CanvasContextKind::Mesh2d) ||
        (vertex_count && !vertices) || (index_count && !indices) ||
        (command_count && !commands) ||
        vertex_count > OOS_GFX_MAX_VERTICES ||
        index_count > OOS_GFX_MAX_INDICES ||
        command_count > OOS_GFX_MAX_DRAW_COMMANDS) {
      error = "mesh canvas batch is invalid";
      return false;
    }
    for (size_t index = 0; index < index_count; ++index) {
      if (indices[index] >= vertex_count) {
        error = "mesh canvas index exceeds its vertex batch";
        return false;
      }
    }
    std::vector<OosGfxDrawCommand> translated;
    try {
      translated.reserve(command_count);
    } catch (const std::bad_alloc &) {
      error = "retain mesh canvas commands failed";
      return false;
    }
    for (size_t index = 0; index < command_count; ++index) {
      if (commands[index].first_index > index_count ||
          commands[index].index_count >
              index_count - commands[index].first_index) {
        error = "mesh canvas draw range is invalid";
        return false;
      }
      uint32_t target_texture = 0;
      if (!mapTexture(*canvas, commands[index].texture, target_texture, false)) {
        error = "mesh canvas draw references an unknown texture";
        return false;
      }
      translated.push_back(commands[index]);
      translated.back().texture = target_texture;
    }
    try {
      canvas->vertices.clear();
      canvas->indices.clear();
      if (vertex_count)
        canvas->vertices.assign(vertices, vertices + vertex_count);
      if (index_count)
        canvas->indices.assign(indices, indices + index_count);
      canvas->commands = std::move(translated);
    } catch (const std::bad_alloc &) {
      error = "retain mesh canvas batch failed";
      return false;
    }
    canvas->clear_rgba = clear_rgba;
    canvas->has_frame = true;
    dirty = true;
    return true;
  }

  Canvas *glesCanvas(uint32_t handle) {
    Canvas *canvas = find(handle);
    if (!canvas || !requireContext(*canvas, CanvasContextKind::Gles2)) {
      if (!canvas)
        error = "GLES canvas handle is invalid";
      return nullptr;
    }
    if (canvas->canvas_texture == 0)
      canvas->canvas_texture = nextTargetTexture();
    return canvas;
  }

  static bool mapHandle(std::unordered_map<uint32_t, uint32_t> &handles,
                        uint32_t guest, uint32_t &target_handle,
                        bool create) {
    const auto found = handles.find(guest);
    if (found != handles.end()) {
      target_handle = found->second;
      return true;
    }
    if (!create || guest == 0)
      return false;
    target_handle = nextTargetTexture();
    handles.emplace(guest, target_handle);
    return true;
  }

  bool glesCapabilities(uint32_t handle, OosGlesCapabilities &result) {
    return glesCanvas(handle) && target.glesCapabilities(result);
  }

  bool glesTextureSet(uint32_t handle, uint32_t texture, uint32_t format,
                      uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      uint32_t row_stride, uint32_t flags,
                      const uint8_t *pixels, size_t pixel_bytes) {
    Canvas *canvas = glesCanvas(handle);
    if (!canvas)
      return false;
    uint32_t target_handle = 0;
    const bool existing =
        mapHandle(canvas->textures, texture, target_handle, false);
    if (!existing && (x != 0 || y != 0)) {
      error = "a new GLES texture requires a full replacement upload";
      return false;
    }
    if (!existing)
      mapHandle(canvas->textures, texture, target_handle, true);
    if (target.setTexture(target_handle, format, x, y, width, height,
                          row_stride,
                          existing ? flags : flags | OOS_TEXTURE_REPLACE,
                          pixels, pixel_bytes))
      return true;
    if (!existing)
      canvas->textures.erase(texture);
    error = "GLES canvas texture upload failed";
    return false;
  }

  bool glesTextureFree(uint32_t handle, uint32_t texture) {
    Canvas *canvas = glesCanvas(handle);
    if (!canvas)
      return false;
    const auto found = canvas->textures.find(texture);
    if (found == canvas->textures.end())
      return true;
    if (!target.freeTexture(found->second)) {
      error = "free GLES canvas texture failed";
      return false;
    }
    canvas->textures.erase(found);
    return true;
  }

  bool glesBufferSet(uint32_t handle, uint32_t buffer, uint32_t size,
                     uint32_t usage, const uint8_t *data, size_t data_size) {
    Canvas *canvas = glesCanvas(handle);
    if (!canvas)
      return false;
    uint32_t target_handle = 0;
    const bool existing =
        mapHandle(canvas->buffers, buffer, target_handle, false);
    if (!existing)
      mapHandle(canvas->buffers, buffer, target_handle, true);
    if (target.setGlesBuffer(target_handle, size, usage, data, data_size))
      return true;
    if (!existing)
      canvas->buffers.erase(buffer);
    error = "set GLES canvas buffer failed";
    return false;
  }

  bool glesBufferWrite(uint32_t handle, uint32_t buffer, uint32_t offset,
                       const uint8_t *data, size_t data_size) {
    Canvas *canvas = glesCanvas(handle);
    uint32_t target_handle = 0;
    if (!canvas ||
        !mapHandle(canvas->buffers, buffer, target_handle, false) ||
        !target.writeGlesBuffer(target_handle, offset, data, data_size)) {
      error = "write GLES canvas buffer failed";
      return false;
    }
    return true;
  }

  bool glesBufferFree(uint32_t handle, uint32_t buffer) {
    Canvas *canvas = glesCanvas(handle);
    if (!canvas)
      return false;
    const auto found = canvas->buffers.find(buffer);
    if (found == canvas->buffers.end())
      return true;
    if (!target.freeGlesBuffer(found->second)) {
      error = "free GLES canvas buffer failed";
      return false;
    }
    canvas->buffers.erase(found);
    return true;
  }

  bool glesShaderSet(uint32_t handle, uint32_t shader, uint32_t stage,
                     const char *source, size_t source_size) {
    Canvas *canvas = glesCanvas(handle);
    if (!canvas)
      return false;
    uint32_t target_handle = 0;
    const bool existing =
        mapHandle(canvas->shaders, shader, target_handle, false);
    if (!existing)
      mapHandle(canvas->shaders, shader, target_handle, true);
    if (target.setGlesShader(target_handle, stage, source, source_size))
      return true;
    if (!existing)
      canvas->shaders.erase(shader);
    error = "set GLES canvas shader failed";
    return false;
  }

  bool glesShaderFree(uint32_t handle, uint32_t shader) {
    Canvas *canvas = glesCanvas(handle);
    if (!canvas)
      return false;
    const auto found = canvas->shaders.find(shader);
    if (found == canvas->shaders.end())
      return true;
    if (!target.freeGlesShader(found->second)) {
      error = "free GLES canvas shader failed";
      return false;
    }
    canvas->shaders.erase(found);
    return true;
  }

  bool glesProgramSet(uint32_t handle, uint32_t program,
                      uint32_t vertex_shader, uint32_t fragment_shader) {
    Canvas *canvas = glesCanvas(handle);
    if (!canvas)
      return false;
    uint32_t vertex = 0;
    uint32_t fragment = 0;
    if (!mapHandle(canvas->shaders, vertex_shader, vertex, false) ||
        !mapHandle(canvas->shaders, fragment_shader, fragment, false)) {
      error = "GLES canvas program references an unknown shader";
      return false;
    }
    uint32_t target_handle = 0;
    const bool existing =
        mapHandle(canvas->programs, program, target_handle, false);
    if (!existing)
      mapHandle(canvas->programs, program, target_handle, true);
    if (target.setGlesProgram(target_handle, vertex, fragment))
      return true;
    if (!existing)
      canvas->programs.erase(program);
    error = "set GLES canvas program failed";
    return false;
  }

  bool glesProgramFree(uint32_t handle, uint32_t program) {
    Canvas *canvas = glesCanvas(handle);
    if (!canvas)
      return false;
    const auto found = canvas->programs.find(program);
    if (found == canvas->programs.end())
      return true;
    if (!target.freeGlesProgram(found->second)) {
      error = "free GLES canvas program failed";
      return false;
    }
    canvas->programs.erase(found);
    return true;
  }

  int32_t glesLocation(uint32_t handle, uint32_t program, const char *name,
                       size_t name_size, bool attribute) {
    Canvas *canvas = glesCanvas(handle);
    uint32_t target_handle = 0;
    if (!canvas ||
        !mapHandle(canvas->programs, program, target_handle, false))
      return -1;
    return attribute
               ? target.glesAttributeLocation(target_handle, name, name_size)
               : target.glesUniformLocation(target_handle, name, name_size);
  }

  bool glesSubmit(uint32_t handle, const OosGlesCommand *commands,
                  size_t command_count, const uint32_t *data,
                  size_t data_words) {
    Canvas *canvas = glesCanvas(handle);
    if (!canvas || !commands || command_count < 2 ||
        command_count > OOS_GLES_MAX_COMMANDS ||
        data_words > OOS_GLES_MAX_COMMAND_DATA_WORDS ||
        (data_words != 0 && !data))
      return false;
    std::vector<OosGlesCommand> translated(commands, commands + command_count);
    for (OosGlesCommand &command : translated) {
      std::unordered_map<uint32_t, uint32_t> *resources = nullptr;
      uint32_t argument = 0;
      switch (command.opcode) {
      case OOS_GLES_USE_PROGRAM:
        resources = &canvas->programs;
        break;
      case OOS_GLES_BIND_TEXTURE:
        resources = &canvas->textures;
        argument = 1;
        break;
      case OOS_GLES_BIND_VERTEX_BUFFER:
      case OOS_GLES_BIND_INDEX_BUFFER:
        resources = &canvas->buffers;
        break;
      default:
        continue;
      }
      uint32_t target_handle = 0;
      if (!mapHandle(*resources, command.args[argument], target_handle,
                     false)) {
        error = "GLES canvas command references an unknown resource";
        return false;
      }
      command.args[argument] = target_handle;
    }
    if (!target.submitGlesToTexture(
            canvas->canvas_texture, canvas->render_width,
            canvas->render_height, translated.data(), translated.size(),
            data, data_words)) {
      error = "render GLES canvas to its compositor texture failed";
      return false;
    }
    canvas->texture_live = true;
    canvas->has_frame = true;
    dirty = true;
    return true;
  }

  bool uploadCanvas2d(Canvas &canvas) {
    if (!canvas.pixels_dirty)
      return true;
    const bool success = target.setTexture(
        canvas.canvas_texture, OOS_TEXTURE_RGBA8888, 0, 0,
        canvas.render_width, canvas.render_height,
        canvas.render_width * 4,
        canvas.texture_live ? 0u
                            : static_cast<uint32_t>(
                                  OOS_TEXTURE_REPLACE |
                                  OOS_TEXTURE_LINEAR_MINIFICATION |
                                  OOS_TEXTURE_LINEAR_MAGNIFICATION),
        canvas.renderer->pixels(), canvas.renderer->pixelBytes());
    if (!success) {
      error = "upload Canvas2D frame failed";
      return false;
    }
    canvas.texture_live = true;
    canvas.pixels_dirty = false;
    return true;
  }

  bool present() {
    if (!dirty)
      return true;
    std::vector<std::pair<uint32_t, Canvas *>> ordered;
    for (auto &entry : canvases) {
      if (entry.second.geometry.visible && entry.second.has_frame)
        ordered.emplace_back(entry.first, &entry.second);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto &left,
                                                  const auto &right) {
      if (left.second->geometry.z_order != right.second->geometry.z_order)
        return left.second->geometry.z_order < right.second->geometry.z_order;
      return left.first < right.first;
    });

    size_t vertex_count = 0;
    size_t index_count = 0;
    size_t command_count = 0;
    for (auto &entry : ordered) {
      Canvas &canvas = *entry.second;
      if (canvas.context == CanvasContextKind::Canvas2d ||
          canvas.context == CanvasContextKind::Gles2) {
        if (canvas.context == CanvasContextKind::Canvas2d &&
            !uploadCanvas2d(canvas))
          return false;
        vertex_count += 4;
        index_count += 6;
        command_count += 1;
      } else if (canvas.context == CanvasContextKind::Mesh2d) {
        vertex_count += canvas.vertices.size();
        index_count += canvas.indices.size();
        command_count += canvas.commands.size();
      }
    }
    if (vertex_count > OOS_GFX_MAX_VERTICES ||
        vertex_count > std::numeric_limits<uint16_t>::max() ||
        index_count > OOS_GFX_MAX_INDICES ||
        command_count > OOS_GFX_MAX_DRAW_COMMANDS) {
      error = "combined application scene exceeds graphics limits";
      return false;
    }
    std::vector<OosGfxVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<OosGfxDrawCommand> commands;
    vertices.reserve(vertex_count);
    indices.reserve(index_count);
    commands.reserve(command_count);
    uint32_t clear = 0xff000000u;
    const Canvas *root = find(0);
    if (root && root->has_frame)
      clear = root->clear_rgba;
    for (auto &entry : ordered) {
      Canvas &canvas = *entry.second;
      const uint32_t vertex_base = static_cast<uint32_t>(vertices.size());
      const uint32_t index_base = static_cast<uint32_t>(indices.size());
      const float x = static_cast<float>(canvas.geometry.x);
      const float y = static_cast<float>(canvas.geometry.y);
      const float width = static_cast<float>(canvas.geometry.width);
      const float height = static_cast<float>(canvas.geometry.height);
      if (canvas.context == CanvasContextKind::Canvas2d ||
          canvas.context == CanvasContextKind::Gles2) {
        const bool flip_y = canvas.context == CanvasContextKind::Gles2;
        const OosGfxVertex quad[] = {
            {{x, y}, {0, flip_y ? 1.0f : 0.0f}, {255, 255, 255, 255}},
            {{x + width, y}, {1, flip_y ? 1.0f : 0.0f},
             {255, 255, 255, 255}},
            {{x, y + height}, {0, flip_y ? 0.0f : 1.0f},
             {255, 255, 255, 255}},
            {{x + width, y + height}, {1, flip_y ? 0.0f : 1.0f},
             {255, 255, 255, 255}},
        };
        vertices.insert(vertices.end(), std::begin(quad), std::end(quad));
        const uint16_t quad_indices[] = {
            static_cast<uint16_t>(vertex_base),
            static_cast<uint16_t>(vertex_base + 1),
            static_cast<uint16_t>(vertex_base + 2),
            static_cast<uint16_t>(vertex_base + 2),
            static_cast<uint16_t>(vertex_base + 1),
            static_cast<uint16_t>(vertex_base + 3),
        };
        indices.insert(indices.end(), std::begin(quad_indices),
                       std::end(quad_indices));
        commands.push_back({index_base,
                            6,
                            canvas.canvas_texture,
                            {x, y},
                            {x + width, y + height}});
        continue;
      }
      if (canvas.context != CanvasContextKind::Mesh2d)
        continue;
      for (OosGfxVertex vertex : canvas.vertices) {
        const float scale_x = width / canvas.render_width;
        const float scale_y = height / canvas.render_height;
        vertex.position[0] = x + vertex.position[0] * scale_x;
        vertex.position[1] = y + vertex.position[1] * scale_y;
        vertices.push_back(vertex);
      }
      for (uint16_t index : canvas.indices)
        indices.push_back(static_cast<uint16_t>(vertex_base + index));
      for (OosGfxDrawCommand command : canvas.commands) {
        const float scale_x = width / canvas.render_width;
        const float scale_y = height / canvas.render_height;
        command.first_index += index_base;
        command.clip_min[0] = std::max(x, x + command.clip_min[0] * scale_x);
        command.clip_min[1] = std::max(y, y + command.clip_min[1] * scale_y);
        command.clip_max[0] =
            std::min(x + width, x + command.clip_max[0] * scale_x);
        command.clip_max[1] =
            std::min(y + height, y + command.clip_max[1] * scale_y);
        commands.push_back(command);
      }
    }
    const bool success = target.submit(
        vertices.empty() ? nullptr : vertices.data(), vertices.size(),
        indices.empty() ? nullptr : indices.data(), indices.size(),
        commands.empty() ? nullptr : commands.data(), commands.size(), clear);
    if (success)
      dirty = false;
    else
      error = "submit combined application scene failed";
    return success;
  }

  void reset() {
    for (auto &entry : canvases)
      release(entry.second);
    canvases.clear();
    Canvas root;
    root.geometry = {0, 0, target.width(), target.height(), 0, true};
    root.render_width = target.width();
    root.render_height = target.height();
    root.clear_rgba = 0xff000000u;
    canvases.emplace(0, std::move(root));
    dirty = false;
    error.clear();
  }

  GraphicsHost &target;
  resources::FontAssetService fonts;
  Canvas2dRenderer text_metrics;
  std::unordered_map<uint32_t, Canvas> canvases;
  uint32_t next_canvas = 1;
  std::string error;
  bool dirty = false;
};

ApplicationScene::ApplicationScene(GraphicsHost &target,
                                   std::string font_directory)
    : impl_(std::make_unique<Impl>(target, std::move(font_directory))) {}
ApplicationScene::~ApplicationScene() = default;

uint32_t ApplicationScene::createCanvas(const CanvasGeometry &geometry,
                                        CanvasContextKind context) {
  return impl_->create(geometry, context);
}
bool ApplicationScene::configureCanvas(uint32_t canvas,
                                       const CanvasGeometry &geometry) {
  return impl_->configure(canvas, geometry);
}
bool ApplicationScene::placeCanvas(uint32_t canvas,
                                   const CanvasGeometry &geometry) {
  return impl_->place(canvas, geometry);
}
bool ApplicationScene::destroyCanvas(uint32_t canvas) {
  return impl_->destroy(canvas);
}
bool ApplicationScene::submitCanvas2d(uint32_t canvas,
                                      const Canvas2dCommand *commands,
                                      size_t command_count,
                                      const uint8_t *text, size_t text_size) {
  return impl_->submit2d(canvas, commands, command_count, text, text_size);
}
bool ApplicationScene::setCanvasTexture(
    uint32_t canvas, uint32_t texture, uint32_t format, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, uint32_t row_stride, uint32_t flags,
    const uint8_t *pixels, size_t pixel_bytes) {
  return impl_->textureSet(canvas, texture, format, x, y, width, height,
                           row_stride, flags, pixels, pixel_bytes);
}
bool ApplicationScene::freeCanvasTexture(uint32_t canvas, uint32_t texture) {
  return impl_->textureFree(canvas, texture);
}
bool ApplicationScene::submitCanvasMesh(
    uint32_t canvas, const OosGfxVertex *vertices, size_t vertex_count,
    const uint16_t *indices, size_t index_count,
    const OosGfxDrawCommand *commands, size_t command_count,
    uint32_t clear_rgba) {
  return impl_->submitMesh(canvas, vertices, vertex_count, indices, index_count,
                           commands, command_count, clear_rgba);
}
bool ApplicationScene::canvasGlesCapabilities(
    uint32_t canvas, OosGlesCapabilities &result) {
  return impl_->glesCapabilities(canvas, result);
}
bool ApplicationScene::setCanvasGlesTexture(
    uint32_t canvas, uint32_t texture, uint32_t format, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, uint32_t row_stride, uint32_t flags,
    const uint8_t *pixels, size_t pixel_bytes) {
  return impl_->glesTextureSet(canvas, texture, format, x, y, width, height,
                               row_stride, flags, pixels, pixel_bytes);
}
bool ApplicationScene::freeCanvasGlesTexture(uint32_t canvas,
                                             uint32_t texture) {
  return impl_->glesTextureFree(canvas, texture);
}
bool ApplicationScene::setCanvasGlesBuffer(
    uint32_t canvas, uint32_t buffer, uint32_t size, uint32_t usage,
    const uint8_t *data, size_t data_size) {
  return impl_->glesBufferSet(canvas, buffer, size, usage, data, data_size);
}
bool ApplicationScene::writeCanvasGlesBuffer(
    uint32_t canvas, uint32_t buffer, uint32_t offset, const uint8_t *data,
    size_t data_size) {
  return impl_->glesBufferWrite(canvas, buffer, offset, data, data_size);
}
bool ApplicationScene::freeCanvasGlesBuffer(uint32_t canvas,
                                            uint32_t buffer) {
  return impl_->glesBufferFree(canvas, buffer);
}
bool ApplicationScene::setCanvasGlesShader(uint32_t canvas, uint32_t shader,
                                           uint32_t stage, const char *source,
                                           size_t source_size) {
  return impl_->glesShaderSet(canvas, shader, stage, source, source_size);
}
bool ApplicationScene::freeCanvasGlesShader(uint32_t canvas,
                                            uint32_t shader) {
  return impl_->glesShaderFree(canvas, shader);
}
bool ApplicationScene::setCanvasGlesProgram(uint32_t canvas, uint32_t program,
                                            uint32_t vertex_shader,
                                            uint32_t fragment_shader) {
  return impl_->glesProgramSet(canvas, program, vertex_shader, fragment_shader);
}
bool ApplicationScene::freeCanvasGlesProgram(uint32_t canvas,
                                             uint32_t program) {
  return impl_->glesProgramFree(canvas, program);
}
int32_t ApplicationScene::canvasGlesAttributeLocation(
    uint32_t canvas, uint32_t program, const char *name, size_t name_size) {
  return impl_->glesLocation(canvas, program, name, name_size, true);
}
int32_t ApplicationScene::canvasGlesUniformLocation(
    uint32_t canvas, uint32_t program, const char *name, size_t name_size) {
  return impl_->glesLocation(canvas, program, name, name_size, false);
}
bool ApplicationScene::submitCanvasGles(uint32_t canvas,
                                        const OosGlesCommand *commands,
                                        size_t command_count,
                                        const uint32_t *data,
                                        size_t data_words) {
  return impl_->glesSubmit(canvas, commands, command_count, data, data_words);
}
bool ApplicationScene::present() { return impl_->present(); }
float ApplicationScene::measureText(const char *text, size_t text_size,
                                    float font_size) {
  return impl_->text_metrics.measureText(text, text_size, font_size);
}
void ApplicationScene::reset() { impl_->reset(); }
const std::string &ApplicationScene::lastError() const { return impl_->error; }

uint32_t ApplicationScene::width() const { return impl_->target.width(); }
uint32_t ApplicationScene::height() const { return impl_->target.height(); }
float ApplicationScene::pixelsPerPoint() const {
  return impl_->target.pixelsPerPoint();
}
uint32_t ApplicationScene::surfaceFormat() const {
  return impl_->target.surfaceFormat();
}
uint32_t ApplicationScene::supportedTextureFormats() const {
  return impl_->target.supportedTextureFormats();
}
bool ApplicationScene::setTexture(uint32_t texture, uint32_t format,
                                  uint32_t x, uint32_t y, uint32_t width,
                                  uint32_t height, uint32_t row_stride,
                                  uint32_t flags, const uint8_t *pixels,
                                  size_t pixel_bytes) {
  return setCanvasTexture(0, texture, format, x, y, width, height, row_stride,
                          flags, pixels, pixel_bytes);
}
bool ApplicationScene::freeTexture(uint32_t texture) {
  return freeCanvasTexture(0, texture);
}
bool ApplicationScene::submit(const OosGfxVertex *vertices,
                              size_t vertex_count, const uint16_t *indices,
                              size_t index_count,
                              const OosGfxDrawCommand *commands,
                              size_t command_count, uint32_t clear_rgba) {
  return submitCanvasMesh(0, vertices, vertex_count, indices, index_count,
                          commands, command_count, clear_rgba);
}

bool ApplicationScene::glesCapabilities(OosGlesCapabilities &result) {
  return canvasGlesCapabilities(0, result);
}
bool ApplicationScene::setGlesBuffer(uint32_t buffer, uint32_t size,
                                     uint32_t usage, const uint8_t *data,
                                     size_t data_size) {
  return setCanvasGlesBuffer(0, buffer, size, usage, data, data_size);
}
bool ApplicationScene::writeGlesBuffer(uint32_t buffer, uint32_t offset,
                                       const uint8_t *data, size_t data_size) {
  return writeCanvasGlesBuffer(0, buffer, offset, data, data_size);
}
bool ApplicationScene::freeGlesBuffer(uint32_t buffer) {
  return freeCanvasGlesBuffer(0, buffer);
}
bool ApplicationScene::setGlesShader(uint32_t shader, uint32_t stage,
                                     const char *source, size_t source_size) {
  return setCanvasGlesShader(0, shader, stage, source, source_size);
}
bool ApplicationScene::freeGlesShader(uint32_t shader) {
  return freeCanvasGlesShader(0, shader);
}
bool ApplicationScene::setGlesProgram(uint32_t program, uint32_t vertex_shader,
                                      uint32_t fragment_shader) {
  return setCanvasGlesProgram(0, program, vertex_shader, fragment_shader);
}
bool ApplicationScene::freeGlesProgram(uint32_t program) {
  return freeCanvasGlesProgram(0, program);
}
int32_t ApplicationScene::glesAttributeLocation(uint32_t program,
                                                const char *name,
                                                size_t name_size) {
  return canvasGlesAttributeLocation(0, program, name, name_size);
}
int32_t ApplicationScene::glesUniformLocation(uint32_t program,
                                              const char *name,
                                              size_t name_size) {
  return canvasGlesUniformLocation(0, program, name, name_size);
}
bool ApplicationScene::submitGles(const OosGlesCommand *commands,
                                  size_t command_count, const uint32_t *data,
                                  size_t data_words) {
  return submitCanvasGles(0, commands, command_count, data, data_words);
}

} // namespace oos::runtime
