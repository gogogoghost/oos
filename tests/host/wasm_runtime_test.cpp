#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "oos/runtime/graphics_host.h"
#include "oos/runtime/native_app_manager.h"

namespace {

class FakeGraphics final : public oos::runtime::GraphicsHost {
public:
  uint32_t width() const override { return 240; }
  uint32_t height() const override { return 320; }

  bool setTexture(uint32_t texture, uint32_t x, uint32_t y, uint32_t width,
                  uint32_t height, uint32_t flags, const uint8_t *rgba,
                  size_t rgba_size) override {
    if (!texture || !rgba ||
        rgba_size != static_cast<size_t>(width) * height * 4)
      return false;
    auto found = textures.find(texture);
    if (found == textures.end()) {
      if (x || y)
        return false;
      textures.emplace(texture, Size{width, height});
    } else if ((flags & OOS_TEXTURE_REPLACE) != 0) {
      if (x || y)
        return false;
      found->second = Size{width, height};
    } else if (x + width > found->second.width ||
               y + height > found->second.height) {
      return false;
    }
    ++texture_updates;
    return true;
  }

  bool freeTexture(uint32_t texture) override {
    textures.erase(texture);
    return true;
  }

  bool submit(const OosGfxVertex *vertices, size_t vertex_count,
              const uint16_t *indices, size_t index_count,
              const OosGfxDrawCommand *commands, size_t command_count,
              uint32_t) override {
    if (!vertices || !indices || !commands || !vertex_count || !index_count ||
        !command_count)
      return false;
    for (size_t index = 0; index < index_count; ++index) {
      if (indices[index] >= vertex_count)
        return false;
    }
    for (size_t index = 0; index < command_count; ++index) {
      const auto &command = commands[index];
      if (!textures.count(command.texture) ||
          command.first_index > index_count ||
          command.index_count > index_count - command.first_index)
        return false;
    }
    last_vertices = vertex_count;
    last_indices = index_count;
    last_commands = command_count;
    ++frames;
    return true;
  }

  struct Size {
    uint32_t width;
    uint32_t height;
  };
  std::unordered_map<uint32_t, Size> textures;
  size_t texture_updates = 0;
  size_t frames = 0;
  size_t last_vertices = 0;
  size_t last_indices = 0;
  size_t last_commands = 0;
};

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s launcher.wasm\n", argv[0]);
    return 2;
  }
  FakeGraphics graphics;
  oos::runtime::NativeAppManager apps(graphics);
  for (size_t index = 0; index < 3; ++index) {
    char id[16] = {};
    std::snprintf(id, sizeof(id), "app-%zu", index);
    if (!apps.load(id, argv[1]) || !apps.activate(id) ||
        !apps.render(1'000'000 + index * 10'000)) {
      std::fprintf(stderr, "app %zu initial frame failed: %s\n", index,
                   apps.lastError());
      return 1;
    }
  }
  if (apps.load("app-over-limit", argv[1]) || !apps.activate("app-0"))
    return 1;
  oos::input::KeyEvent ok;
  ok.code = 352;
  ok.action = oos::input::KeyAction::Pressed;
  if (!apps.dispatchKey(ok, 1'100'000) || !apps.render(1'200'000)) {
    std::fprintf(stderr, "apps frame failed: %s\n", apps.lastError());
    return 1;
  }
  oos::input::KeyEvent right;
  right.code = 106;
  right.action = oos::input::KeyAction::Pressed;
  if (!apps.dispatchKey(right, 1'300'000) || !apps.render(1'400'000)) {
    std::fprintf(stderr, "navigation frame failed: %s\n", apps.lastError());
    return 1;
  }
  const size_t resident_textures = graphics.textures.size();
  std::printf("WAMR egui integration passed: apps=%zu frames=%zu textures=%zu "
              "updates=%zu vertices=%zu indices=%zu commands=%zu\n",
              apps.residentCount(), graphics.frames, resident_textures,
              graphics.texture_updates, graphics.last_vertices,
              graphics.last_indices, graphics.last_commands);
  apps.shutdown();
  return graphics.frames == 5 && resident_textures == 3 &&
                 graphics.textures.empty() && graphics.texture_updates > 0
             ? 0
             : 1;
}
