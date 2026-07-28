#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "oos/device/device.h"
#include "oos/input/key_input.h"
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

class MockDevice final : public oos::device::Device {
public:
  const oos::device::DeviceDescriptor &descriptor() const override {
    static constexpr oos::device::DeviceDescriptor descriptor = {
        "local", "OOS", "Local Test Device", 0, 240, 320, 0, 0};
    return descriptor;
  }

  const oos::device::ServiceConfiguration &services() const override {
    static constexpr oos::device::ServiceConfiguration services = {
        "local",      "mock:wlan0",    "mock:bluetooth", "mock:modem",
        "mock:power", "mock:vibrator", "mock:camera",    true};
    return services;
  }

  oos::device::CapabilityState
  capability(oos::device::Feature feature) const override {
    return feature == oos::device::Feature::PrimaryDisplay
               ? oos::device::CapabilityState::Validated
               : oos::device::CapabilityState::Implemented;
  }

  bool initialize(const oos::device::DeviceInitOptions &) override {
    return true;
  }
  void shutdown() override {}
  oos::device::Display &display() override { std::abort(); }
  oos::input::KeyInputSource &keyInput() override { std::abort(); }
  const std::string &lastError() const override { return error_; }

private:
  std::string error_;
};

} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s launcher.wasm wit-smoke.wasm\n", argv[0]);
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
  oos::runtime::NativeAppManager wit_smoke(graphics, 1);
  if (!wit_smoke.load("wit-smoke", argv[2]) ||
      !wit_smoke.activate("wit-smoke") || !wit_smoke.render(1'500'000)) {
    std::fprintf(stderr, "WIT device API smoke failed: %s\n",
                 wit_smoke.lastError());
    return 1;
  }
  wit_smoke.shutdown();
  std::printf("WAMR WIT device API imports passed\n");
  MockDevice mock_device;
  oos::runtime::NativeAppManager mock_smoke(graphics, mock_device, 1);
  if (!mock_smoke.load("local-mock", argv[2]) ||
      !mock_smoke.activate("local-mock") || !mock_smoke.render(1'600'000)) {
    std::fprintf(stderr, "WIT local mock API smoke failed: %s\n",
                 mock_smoke.lastError());
    return 1;
  }
  mock_smoke.shutdown();
  std::printf("WAMR WIT local mock API imports passed\n");
  return graphics.frames == 5 && resident_textures == 3 &&
                 graphics.textures.empty() && graphics.texture_updates > 0
             ? 0
             : 1;
}
