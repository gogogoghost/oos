#include "oos/compositor/compositor.h"

#include "oos/device/display.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace oos::compositor {

class LayerSurface::Impl {
public:
  explicit Impl(LayerSurfaceConfig value, uint64_t surface_id)
      : config(std::move(value)), id(surface_id) {}

  LayerSurfaceConfig config;
  uint64_t id;
  std::unordered_map<uint32_t, uint32_t> textures;
  std::vector<OosGfxVertex> vertices;
  std::vector<uint16_t> indices;
  std::vector<OosGfxDrawCommand> commands;
  uint32_t clear_rgba = 0xff000000u;
  bool visible = true;
  bool has_frame = false;
};

class Compositor::Impl {
public:
  std::vector<LayerSurface *> layers;
  uint64_t next_surface_id = 1;
  uint32_t next_texture_id = 0x40000000u;
  bool dirty = false;
};

Compositor::Compositor(device::Display &display)
    : display_(display), impl_(std::make_unique<Impl>()) {}

Compositor::~Compositor() = default;

std::unique_ptr<LayerSurface>
Compositor::createLayer(const LayerSurfaceConfig &config) {
  if (config.name.empty() || config.width == 0 || config.height == 0 ||
      config.x < 0 || config.y < 0 ||
      static_cast<uint64_t>(config.x) + config.width > width() ||
      static_cast<uint64_t>(config.y) + config.height > height())
    return nullptr;
  auto layer = std::unique_ptr<LayerSurface>(
      new LayerSurface(*this, config, impl_->next_surface_id++));
  impl_->layers.push_back(layer.get());
  impl_->dirty = true;
  return layer;
}

bool Compositor::dirty() const { return impl_->dirty; }

bool Compositor::compose() {
  std::vector<LayerSurface *> layers;
  for (LayerSurface *layer : impl_->layers) {
    if (layer->impl_->visible && layer->impl_->has_frame)
      layers.push_back(layer);
  }
  std::sort(layers.begin(), layers.end(),
            [](const LayerSurface *left, const LayerSurface *right) {
              if (left->impl_->config.z_order != right->impl_->config.z_order)
                return left->impl_->config.z_order <
                       right->impl_->config.z_order;
              return left->impl_->id < right->impl_->id;
            });

  size_t vertex_count = 0;
  size_t index_count = 0;
  size_t command_count = 0;
  for (const LayerSurface *layer : layers) {
    vertex_count += layer->impl_->vertices.size();
    index_count += layer->impl_->indices.size();
    command_count += layer->impl_->commands.size();
  }
  if (vertex_count > std::numeric_limits<uint16_t>::max() ||
      vertex_count > OOS_GFX_MAX_VERTICES ||
      index_count > OOS_GFX_MAX_INDICES ||
      command_count > OOS_GFX_MAX_DRAW_COMMANDS)
    return false;

  std::vector<OosGfxVertex> vertices;
  std::vector<uint16_t> indices;
  std::vector<OosGfxDrawCommand> commands;
  vertices.reserve(vertex_count);
  indices.reserve(index_count);
  commands.reserve(command_count);
  uint32_t clear_rgba = 0xff000000u;
  bool have_clear = false;
  for (const LayerSurface *layer : layers) {
    const auto &frame = *layer->impl_;
    if (!have_clear) {
      clear_rgba = frame.clear_rgba;
      have_clear = true;
    }
    const uint32_t vertex_base = static_cast<uint32_t>(vertices.size());
    const uint32_t index_base = static_cast<uint32_t>(indices.size());
    for (OosGfxVertex vertex : frame.vertices) {
      vertex.position[0] += static_cast<float>(frame.config.x);
      vertex.position[1] += static_cast<float>(frame.config.y);
      vertices.push_back(vertex);
    }
    for (uint16_t index : frame.indices)
      indices.push_back(static_cast<uint16_t>(index + vertex_base));
    for (OosGfxDrawCommand command : frame.commands) {
      command.first_index += index_base;
      command.clip_min[0] =
          std::max(static_cast<float>(frame.config.x),
                   command.clip_min[0] + static_cast<float>(frame.config.x));
      command.clip_min[1] =
          std::max(static_cast<float>(frame.config.y),
                   command.clip_min[1] + static_cast<float>(frame.config.y));
      command.clip_max[0] =
          std::min(static_cast<float>(frame.config.x + frame.config.width),
                   command.clip_max[0] + static_cast<float>(frame.config.x));
      command.clip_max[1] =
          std::min(static_cast<float>(frame.config.y + frame.config.height),
                   command.clip_max[1] + static_cast<float>(frame.config.y));
      commands.push_back(command);
    }
  }
  const bool success = display_.submit(
      vertices.empty() ? nullptr : vertices.data(), vertices.size(),
      indices.empty() ? nullptr : indices.data(), indices.size(),
      commands.empty() ? nullptr : commands.data(), commands.size(),
      clear_rgba);
  if (success)
    impl_->dirty = false;
  return success;
}

bool Compositor::presentSurface(const SurfaceFrame &frame) {
  const bool valid =
      frame.surface_id != 0 && frame.buffer != nullptr &&
      frame.buffer_width != 0 && frame.buffer_height != 0 && frame.x == 0 &&
      frame.y == 0 && frame.width == width() && frame.height == height() &&
      frame.buffer_width == frame.width &&
      frame.buffer_height == frame.height && std::isfinite(frame.opacity) &&
      frame.opacity == 1.0f && frame.z_order == 0;
  if (!valid) {
    if (frame.acquire_fence_fd >= 0)
      close(frame.acquire_fence_fd);
    return false;
  }
  return display_.presentSurface(frame);
}

bool Compositor::detachRenderContext() {
  return display_.detachRenderContext();
}

bool Compositor::attachRenderContext() {
  return display_.attachRenderContext();
}

uint32_t Compositor::width() const { return display_.width(); }

uint32_t Compositor::height() const { return display_.height(); }

uint32_t Compositor::surfaceFormat() const { return display_.surfaceFormat(); }

uint32_t Compositor::supportedTextureFormats() const {
  return display_.supportedTextureFormats();
}

bool Compositor::setTexture(uint32_t texture, uint32_t format, uint32_t x,
                            uint32_t y, uint32_t width, uint32_t height,
                            uint32_t row_stride, uint32_t flags,
                            const uint8_t *pixels, size_t pixel_bytes) {
  return display_.setTexture(texture, format, x, y, width, height, row_stride,
                             flags, pixels, pixel_bytes);
}

bool Compositor::freeTexture(uint32_t texture) {
  return display_.freeTexture(texture);
}

bool Compositor::submit(const OosGfxVertex *vertices, size_t vertex_count,
                        const uint16_t *indices, size_t index_count,
                        const OosGfxDrawCommand *commands, size_t command_count,
                        uint32_t clear_rgba) {
  return display_.submit(vertices, vertex_count, indices, index_count, commands,
                         command_count, clear_rgba);
}

bool Compositor::glesCapabilities(OosGlesCapabilities &result) {
  return display_.glesCapabilities(result);
}

bool Compositor::setGlesBuffer(uint32_t buffer, uint32_t size, uint32_t usage,
                               const uint8_t *data, size_t data_size) {
  return display_.setGlesBuffer(buffer, size, usage, data, data_size);
}

bool Compositor::writeGlesBuffer(uint32_t buffer, uint32_t offset,
                                 const uint8_t *data, size_t data_size) {
  return display_.writeGlesBuffer(buffer, offset, data, data_size);
}

bool Compositor::freeGlesBuffer(uint32_t buffer) {
  return display_.freeGlesBuffer(buffer);
}

bool Compositor::setGlesShader(uint32_t shader, uint32_t stage,
                               const char *source, size_t source_size) {
  return display_.setGlesShader(shader, stage, source, source_size);
}

bool Compositor::freeGlesShader(uint32_t shader) {
  return display_.freeGlesShader(shader);
}

bool Compositor::setGlesProgram(uint32_t program, uint32_t vertex_shader,
                                uint32_t fragment_shader) {
  return display_.setGlesProgram(program, vertex_shader, fragment_shader);
}

bool Compositor::freeGlesProgram(uint32_t program) {
  return display_.freeGlesProgram(program);
}

int32_t Compositor::glesAttributeLocation(uint32_t program, const char *name,
                                          size_t name_size) {
  return display_.glesAttributeLocation(program, name, name_size);
}

int32_t Compositor::glesUniformLocation(uint32_t program, const char *name,
                                        size_t name_size) {
  return display_.glesUniformLocation(program, name, name_size);
}

bool Compositor::submitGles(const OosGlesCommand *commands,
                            size_t command_count, const uint32_t *data,
                            size_t data_words) {
  return display_.submitGles(commands, command_count, data, data_words);
}

LayerSurface::LayerSurface(Compositor &compositor,
                           const LayerSurfaceConfig &config, uint64_t id)
    : compositor_(compositor), impl_(std::make_unique<Impl>(config, id)) {}

LayerSurface::~LayerSurface() {
  for (const auto &texture : impl_->textures)
    compositor_.display_.freeTexture(texture.second);
  auto &layers = compositor_.impl_->layers;
  layers.erase(std::remove(layers.begin(), layers.end(), this), layers.end());
  compositor_.impl_->dirty = true;
}

const std::string &LayerSurface::name() const { return impl_->config.name; }

int32_t LayerSurface::zOrder() const { return impl_->config.z_order; }

void LayerSurface::setVisible(bool visible) {
  if (impl_->visible != visible) {
    impl_->visible = visible;
    compositor_.impl_->dirty = true;
  }
}

bool LayerSurface::visible() const { return impl_->visible; }

void LayerSurface::clearFrame() {
  impl_->vertices.clear();
  impl_->indices.clear();
  impl_->commands.clear();
  impl_->has_frame = false;
  compositor_.impl_->dirty = true;
}

uint32_t LayerSurface::width() const { return impl_->config.width; }

uint32_t LayerSurface::height() const { return impl_->config.height; }

uint32_t LayerSurface::surfaceFormat() const {
  return compositor_.display_.surfaceFormat();
}

uint32_t LayerSurface::supportedTextureFormats() const {
  return compositor_.display_.supportedTextureFormats();
}

bool LayerSurface::setTexture(uint32_t texture, uint32_t format, uint32_t x,
                              uint32_t y, uint32_t texture_width,
                              uint32_t texture_height, uint32_t row_stride,
                              uint32_t flags, const uint8_t *pixels,
                              size_t pixel_bytes) {
  if (texture == 0)
    return false;
  auto found = impl_->textures.find(texture);
  if (found == impl_->textures.end()) {
    if ((flags & OOS_TEXTURE_REPLACE) == 0)
      return false;
    uint32_t physical = compositor_.impl_->next_texture_id++;
    if (physical == 0)
      physical = compositor_.impl_->next_texture_id++;
    found = impl_->textures.emplace(texture, physical).first;
  }
  return compositor_.display_.setTexture(
      found->second, format, x, y, texture_width, texture_height, row_stride,
      flags, pixels, pixel_bytes);
}

bool LayerSurface::freeTexture(uint32_t texture) {
  const auto found = impl_->textures.find(texture);
  if (found == impl_->textures.end())
    return true;
  const bool success = compositor_.display_.freeTexture(found->second);
  if (success)
    impl_->textures.erase(found);
  return success;
}

bool LayerSurface::submit(const OosGfxVertex *vertices, size_t vertex_count,
                          const uint16_t *indices, size_t index_count,
                          const OosGfxDrawCommand *commands,
                          size_t command_count, uint32_t clear_rgba) {
  if ((vertex_count && !vertices) || (index_count && !indices) ||
      (command_count && !commands) || vertex_count > OOS_GFX_MAX_VERTICES ||
      index_count > OOS_GFX_MAX_INDICES ||
      command_count > OOS_GFX_MAX_DRAW_COMMANDS)
    return false;
  std::vector<OosGfxDrawCommand> mapped;
  mapped.reserve(command_count);
  for (size_t index = 0; index < command_count; ++index) {
    OosGfxDrawCommand command = commands[index];
    const auto texture = impl_->textures.find(command.texture);
    if (texture == impl_->textures.end() || command.first_index > index_count ||
        command.index_count > index_count - command.first_index)
      return false;
    command.texture = texture->second;
    mapped.push_back(command);
  }
  for (size_t index = 0; index < index_count; ++index) {
    if (indices[index] >= vertex_count)
      return false;
  }
  if (vertex_count)
    impl_->vertices.assign(vertices, vertices + vertex_count);
  else
    impl_->vertices.clear();
  if (index_count)
    impl_->indices.assign(indices, indices + index_count);
  else
    impl_->indices.clear();
  impl_->commands = std::move(mapped);
  impl_->clear_rgba = clear_rgba;
  impl_->has_frame = true;
  compositor_.impl_->dirty = true;
  return true;
}

bool LayerSurface::glesCapabilities(OosGlesCapabilities &) { return false; }
bool LayerSurface::setGlesBuffer(uint32_t, uint32_t, uint32_t, const uint8_t *,
                                 size_t) {
  return false;
}
bool LayerSurface::writeGlesBuffer(uint32_t, uint32_t, const uint8_t *,
                                   size_t) {
  return false;
}
bool LayerSurface::freeGlesBuffer(uint32_t) { return false; }
bool LayerSurface::setGlesShader(uint32_t, uint32_t, const char *, size_t) {
  return false;
}
bool LayerSurface::freeGlesShader(uint32_t) { return false; }
bool LayerSurface::setGlesProgram(uint32_t, uint32_t, uint32_t) {
  return false;
}
bool LayerSurface::freeGlesProgram(uint32_t) { return false; }
int32_t LayerSurface::glesAttributeLocation(uint32_t, const char *, size_t) {
  return -1;
}
int32_t LayerSurface::glesUniformLocation(uint32_t, const char *, size_t) {
  return -1;
}
bool LayerSurface::submitGles(const OosGlesCommand *, size_t, const uint32_t *,
                              size_t) {
  return false;
}

} // namespace oos::compositor
