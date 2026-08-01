#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include "oos/device/device.h"
#include "oos/input/key_input.h"
#include "oos/runtime/graphics_host.h"
#include "oos/runtime/native_app_manager.h"
#include "oos/apps/permissions.h"

namespace {

class FakeGraphics final : public oos::runtime::GraphicsHost {
public:
  uint32_t width() const override { return 240; }
  uint32_t height() const override { return 320; }
  uint32_t surfaceFormat() const override { return OOS_TEXTURE_RGB565; }
  uint32_t supportedTextureFormats() const override {
    return OOS_TEXTURE_FORMAT_MASK;
  }

  bool setTexture(uint32_t texture, uint32_t format, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height, uint32_t row_stride,
                  uint32_t flags, const uint8_t *pixels,
                  size_t pixel_bytes) override {
    const uint32_t bytes_per_pixel = oosTextureBytesPerPixel(format);
    if (!texture || !pixels || !bytes_per_pixel ||
        row_stride < width * bytes_per_pixel ||
        pixel_bytes != static_cast<size_t>(row_stride) * (height - 1) +
                           width * bytes_per_pixel)
      return false;
    auto found = textures.find(texture);
    if (found == textures.end()) {
      if (x || y)
        return false;
      textures.emplace(texture, Size{width, height, format});
    } else if ((flags & OOS_TEXTURE_REPLACE) != 0) {
      if (x || y)
        return false;
      found->second = Size{width, height, format};
    } else if (format != found->second.format ||
               x + width > found->second.width ||
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

  bool glesCapabilities(OosGlesCapabilities &result) override {
    result = {2,
              0,
              2048,
              8,
              8,
              8,
              128,
              64,
              16,
              8,
              OOS_GLES_MAX_BUFFER_BYTES,
              OOS_GLES_MAX_COMMANDS,
              OOS_GLES_MAX_COMMAND_DATA_WORDS};
    return true;
  }

  bool setGlesBuffer(uint32_t buffer, uint32_t size, uint32_t, const uint8_t *,
                     size_t data_size) override {
    if (!buffer || !size || (data_size && data_size != size))
      return false;
    buffers.insert(buffer);
    return true;
  }
  bool writeGlesBuffer(uint32_t buffer, uint32_t, const uint8_t *,
                       size_t data_size) override {
    return buffers.count(buffer) && data_size;
  }
  bool freeGlesBuffer(uint32_t buffer) override {
    buffers.erase(buffer);
    return true;
  }
  bool setGlesShader(uint32_t shader, uint32_t, const char *,
                     size_t source_size) override {
    if (!shader || !source_size)
      return false;
    shaders.insert(shader);
    return true;
  }
  bool freeGlesShader(uint32_t shader) override {
    shaders.erase(shader);
    return true;
  }
  bool setGlesProgram(uint32_t program, uint32_t vertex_shader,
                      uint32_t fragment_shader) override {
    if (!program || !shaders.count(vertex_shader) ||
        !shaders.count(fragment_shader))
      return false;
    programs.insert(program);
    return true;
  }
  bool freeGlesProgram(uint32_t program) override {
    programs.erase(program);
    return true;
  }
  int32_t glesAttributeLocation(uint32_t program, const char *,
                                size_t name_size) override {
    return programs.count(program) && name_size ? 0 : -1;
  }
  int32_t glesUniformLocation(uint32_t program, const char *,
                              size_t name_size) override {
    return programs.count(program) && name_size ? 1 : -1;
  }
  bool submitGles(const OosGlesCommand *commands, size_t command_count,
                  const uint32_t *, size_t) override {
    if (!commands || command_count < 2 ||
        commands[0].opcode != OOS_GLES_BEGIN_FRAME ||
        commands[command_count - 1].opcode != OOS_GLES_END_FRAME)
      return false;
    ++gles_frames;
    return true;
  }

  struct Size {
    uint32_t width;
    uint32_t height;
    uint32_t format;
  };
  std::unordered_map<uint32_t, Size> textures;
  size_t texture_updates = 0;
  size_t frames = 0;
  size_t last_vertices = 0;
  size_t last_indices = 0;
  size_t last_commands = 0;
  size_t gles_frames = 0;
  std::unordered_set<uint32_t> buffers;
  std::unordered_set<uint32_t> shaders;
  std::unordered_set<uint32_t> programs;
};

class MockDevice final : public oos::device::Device {
public:
  explicit MockDevice(const char *id = "local")
      : descriptor_{id, "OOS", "Local Test Device", 0, 240, 320, 0, 0} {}

  const oos::device::DeviceDescriptor &descriptor() const override {
    return descriptor_;
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
  oos::device::DeviceDescriptor descriptor_;
  std::string error_;
};

} // namespace

int main(int argc, char **argv) {
  if (argc != 4) {
    std::fprintf(stderr,
                 "usage: %s egui-demo.wasm wit-smoke.wasm font-directory\n",
                 argv[0]);
    return 2;
  }
  FakeGraphics graphics;
  oos::runtime::NativeAppManager apps(graphics);
  oos::runtime::NativeAppLaunchOptions launcher_launch;
  launcher_launch.module_path = argv[1];
  launcher_launch.font_directory = argv[3];
  for (size_t index = 0; index < 3; ++index) {
    char id[16] = {};
    std::snprintf(id, sizeof(id), "app-%zu", index);
    if (!apps.load(id, launcher_launch) || !apps.activate(id) ||
        !apps.render(1'000'000 + index * 10'000)) {
      std::fprintf(stderr, "app %zu initial frame failed: %s\n", index,
                   apps.lastError());
      return 1;
    }
  }
  if (apps.load("app-over-limit", launcher_launch) || !apps.activate("app-0"))
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
  oos::runtime::NativeAppLaunchOptions smoke_launch;
  smoke_launch.module_path = argv[2];
  smoke_launch.font_directory = argv[3];
  if (!wit_smoke.load("wit-smoke", smoke_launch) ||
      !wit_smoke.activate("wit-smoke") || !wit_smoke.render(1'500'000)) {
    std::fprintf(stderr, "WIT device API smoke failed: %s\n",
                 wit_smoke.lastError());
    return 1;
  }
  wit_smoke.shutdown();
  std::printf("WAMR WIT device/GLES API imports passed: gles_frames=%zu\n",
              graphics.gles_frames);
  MockDevice mock_device;
  oos::runtime::NativeAppManager mock_smoke(graphics, mock_device, 1);
  char storage_template[] = "/tmp/oos-wasm-storage.XXXXXX";
  const char *storage_root = mkdtemp(storage_template);
  if (!storage_root) {
    std::perror("mkdtemp");
    return 1;
  }
  oos::runtime::NativeAppLaunchOptions mock_launch;
  mock_launch.module_path = argv[2];
  mock_launch.data_directory = storage_root;
  mock_launch.system_data_root = storage_root;
  mock_launch.font_directory = argv[3];
  const std::string internal_media = std::string(storage_root) + "/internal";
  const std::string removable_media = std::string(storage_root) + "/removable";
  if (!std::filesystem::create_directories(internal_media) ||
      !std::filesystem::create_directories(removable_media)) {
    std::fprintf(stderr, "cannot create WIT media test roots\n");
    return 1;
  }
  mock_launch.internal_media_directory = internal_media.c_str();
  mock_launch.removable_media_directory = removable_media.c_str();
  const std::vector<std::string> mock_permissions = {
      "audio-capture", "camera",       "power",     "wifi-manage",
      "bluetooth",     "mobileconnection", "device-storage:sdcard",
      "system"};
  mock_launch.service_permission_mask =
      oos::apps::deviceServicePermissionMask(mock_permissions);
  mock_launch.enforce_service_permissions = true;
  if (!mock_smoke.load("local-mock", mock_launch) ||
      !mock_smoke.activate("local-mock") || !mock_smoke.render(1'600'000)) {
    std::fprintf(stderr, "WIT local mock API smoke failed: %s\n",
                 mock_smoke.lastError());
    return 1;
  }
  mock_smoke.shutdown();
  MockDevice denied_device("local-denied");
  oos::runtime::NativeAppManager denied_smoke(graphics, denied_device, 1);
  oos::runtime::NativeAppLaunchOptions denied_launch = mock_launch;
  denied_launch.service_permission_mask = 0;
  if (!denied_smoke.load("permission-denied", denied_launch) ||
      !denied_smoke.activate("permission-denied") ||
      !denied_smoke.render(1'700'000)) {
    std::fprintf(stderr, "WIT permission filtering smoke failed: %s\n",
                 denied_smoke.lastError());
    return 1;
  }
  denied_smoke.shutdown();
  std::filesystem::remove_all(storage_root);
  std::printf("WAMR WIT local mock and permission filtering passed\n");
  return graphics.frames == 5 && resident_textures == 3 &&
                 graphics.textures.empty() && graphics.texture_updates > 0 &&
                 graphics.gles_frames == 3
             ? 0
             : 1;
}
