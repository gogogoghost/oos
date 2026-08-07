#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "oos/apps/permissions.h"
#include "oos/device/device.h"
#include "oos/input/key_input.h"
#include "oos/runtime/graphics_host.h"
#include "oos/runtime/js_app.h"
#include "oos/runtime/native_app_manager.h"
#include "oos/runtime/wasm_app.h"
#include "oos/ui/status_bar_appearance.h"

namespace {

class FakeStatusBar final : public oos::ui::StatusBarAppearanceController {
public:
  void setStatusBarAppearance(oos::ui::StatusBarAppearance next) override {
    appearance = next;
    ++updates;
  }
  oos::ui::StatusBarAppearance statusBarAppearance() const override {
    return appearance;
  }
  bool setSurfaceMode(oos::ui::SurfaceMode next) override {
    surface_mode = next;
    ++surface_mode_updates;
    return true;
  }

  oos::ui::StatusBarAppearance appearance;
  oos::ui::SurfaceMode surface_mode = oos::ui::SurfaceMode::Normal;
  size_t updates = 0;
  size_t surface_mode_updates = 0;
};

class FakeGraphics final : public oos::runtime::GraphicsHost {
public:
  FakeGraphics() : owner_thread_(std::this_thread::get_id()) {}

  uint32_t width() const override {
    checkThread();
    return 240;
  }
  uint32_t height() const override {
    checkThread();
    return 320;
  }
  uint32_t surfaceFormat() const override {
    checkThread();
    return OOS_TEXTURE_RGB565;
  }
  uint32_t supportedTextureFormats() const override {
    checkThread();
    return OOS_TEXTURE_FORMAT_MASK;
  }

  bool threadSafe() const { return !thread_violation_; }

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
  bool submitGlesToTexture(uint32_t texture, uint32_t width, uint32_t height,
                           const OosGlesCommand *commands, size_t command_count,
                           const uint32_t *data, size_t data_words) override {
    if (!texture || !width || !height ||
        !submitGles(commands, command_count, data, data_words))
      return false;
    textures[texture] = Size{width, height, OOS_TEXTURE_RGBA8888};
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

private:
  void checkThread() const {
    if (std::this_thread::get_id() != owner_thread_)
      thread_violation_ = true;
  }

  std::thread::id owner_thread_;
  mutable bool thread_violation_ = false;
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
  if (argc != 13) {
    std::fprintf(stderr,
                 "usage: %s egui-demo wit-smoke thread-smoke "
                 "worker-wit-trap exit-smoke font-directory "
                 "module-parent module-directory production-libc-smoke "
                 "lvgl-demo clay-demo solid-demo.mjs\n",
                 argv[0]);
    return 2;
  }
  FakeGraphics graphics;
  FakeStatusBar status_bar;
  oos::runtime::NativeAppManager apps(graphics);
  oos::runtime::NativeAppLaunchOptions launcher_launch;
  launcher_launch.module_base_path = argv[1];
  launcher_launch.font_directory = argv[6];
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
  oos::runtime::WasmAppOptions libc_options;
  libc_options.font_directory = argv[6];
  oos::runtime::WasmApp libc_smoke(graphics, std::move(libc_options));
  uint32_t libc_delay_ms = 0;
  if (!libc_smoke.load(argv[9]) ||
      libc_smoke.loadedArtifactPath() != std::string(argv[9]) + ".x86_64.aot" ||
      !libc_smoke.initialize() ||
      !libc_smoke.render(1'450'000, libc_delay_ms) || libc_delay_ms != 1000) {
    std::fprintf(stderr, "production C runtime smoke failed: %s\n",
                 libc_smoke.lastError());
    return 1;
  }
  libc_smoke.shutdown();
  std::printf("WAMR production picolibc/libm/TLSF execution passed\n");
  oos::runtime::NativeAppManager wit_smoke(graphics, 1);
  oos::runtime::NativeAppLaunchOptions smoke_launch;
  smoke_launch.module_base_path = argv[2];
  smoke_launch.font_directory = argv[6];
  smoke_launch.status_bar = &status_bar;
  uint32_t smoke_delay_ms = 0;
  if (!wit_smoke.load("wit-smoke", smoke_launch) ||
      !wit_smoke.activate("wit-smoke") ||
      !wit_smoke.render(1'500'000, smoke_delay_ms) || smoke_delay_ms != 1000) {
    std::fprintf(stderr, "WIT device API smoke failed: %s\n",
                 wit_smoke.lastError());
    return 1;
  }
  if (status_bar.updates != 1 ||
      status_bar.appearance != (oos::ui::StatusBarAppearance{0x123456, true}) ||
      status_bar.surface_mode_updates != 2 ||
      status_bar.surface_mode != oos::ui::SurfaceMode::Normal) {
    std::fprintf(stderr, "WIT status bar/surface mode calls failed\n");
    return 1;
  }
  wit_smoke.shutdown();
  std::printf("WAMR WIT device/GLES API imports passed: gles_frames=%zu\n",
              graphics.gles_frames);
  oos::runtime::NativeAppManager thread_smoke(graphics, 1);
  oos::runtime::NativeAppLaunchOptions thread_launch;
  thread_launch.module_base_path = argv[3];
  thread_launch.font_directory = argv[6];
  if (!thread_smoke.load("thread-smoke", thread_launch) ||
      !thread_smoke.activate("thread-smoke") ||
      !thread_smoke.render(1'600'000)) {
    std::fprintf(stderr, "WAMR guest worker smoke failed: %s\n",
                 thread_smoke.lastError());
    return 1;
  }
  thread_smoke.shutdown();
  for (unsigned iteration = 0; iteration < 8; ++iteration) {
    oos::runtime::NativeAppManager repeated_thread(graphics, 1);
    if (!repeated_thread.load("repeated-thread", thread_launch) ||
        !repeated_thread.activate("repeated-thread") ||
        !repeated_thread.render(1'610'000 + iteration * 1'000)) {
      std::fprintf(stderr, "WAMR repeated worker lifecycle failed: %s\n",
                   repeated_thread.lastError());
      return 1;
    }
    repeated_thread.shutdown();
  }
  std::printf("WAMR bounded guest worker passed\n");
  oos::runtime::NativeAppManager affinity_smoke(graphics, 1);
  oos::runtime::NativeAppLaunchOptions affinity_launch;
  affinity_launch.module_base_path = argv[4];
  affinity_launch.font_directory = argv[6];
  if (affinity_smoke.load("worker-wit-trap", affinity_launch) ||
      std::string(affinity_smoke.lastError()).find("guest worker") ==
          std::string::npos) {
    std::fprintf(stderr,
                 "worker WIT thread-affinity trap was not enforced: %s\n",
                 affinity_smoke.lastError());
    return 1;
  }
  std::printf("WAMR worker WIT thread-affinity trap passed\n");
  const std::string unbounded_base = "/tmp/oos-unbounded-memory";
  const std::string unbounded_path = unbounded_base + ".wasm";
  const unsigned char unbounded_module[] = {0x00, 0x61, 0x73, 0x6d, 0x01,
                                            0x00, 0x00, 0x00, 0x05, 0x03,
                                            0x01, 0x00, 0x01};
  {
    std::ofstream output(unbounded_path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(unbounded_module),
                 sizeof(unbounded_module));
  }
  oos::runtime::NativeAppManager policy_test(graphics, 1);
  oos::runtime::NativeAppLaunchOptions unbounded_launch;
  unbounded_launch.module_base_path = unbounded_base.c_str();
  if (policy_test.load("unbounded", unbounded_launch) ||
      std::string(policy_test.lastError()).find("memory") ==
          std::string::npos) {
    std::fprintf(stderr, "unbounded Wasm memory policy was not enforced\n");
    return 1;
  }
  std::filesystem::remove(unbounded_path);
  const std::string oversized_base = "/tmp/oos-oversized-memory";
  const std::string oversized_path = oversized_base + ".wasm";
  const unsigned char oversized_module[] = {0x00, 0x61, 0x73, 0x6d, 0x01,
                                            0x00, 0x00, 0x00, 0x05, 0x05,
                                            0x01, 0x01, 0x01, 0x81, 0x08};
  {
    std::ofstream output(oversized_path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(oversized_module),
                 sizeof(oversized_module));
  }
  oos::runtime::NativeAppLaunchOptions oversized_launch;
  oversized_launch.module_base_path = oversized_base.c_str();
  if (policy_test.load("oversized", oversized_launch) ||
      std::string(policy_test.lastError()).find("memory") ==
          std::string::npos) {
    std::fprintf(stderr, "oversized Wasm memory policy was not enforced\n");
    return 1;
  }
  std::filesystem::remove(oversized_path);
  std::printf("WAMR 64 MiB memory policy passed\n");
  MockDevice mock_device;
  {
    oos::runtime::WasmAppOptions exit_options;
    exit_options.font_directory = argv[6];
    oos::runtime::WasmApp exit_smoke(graphics, mock_device, exit_options);
    if (!exit_smoke.load(argv[5]) || !exit_smoke.initialize() ||
        !exit_smoke.takeExitRequest() || exit_smoke.takeExitRequest()) {
      std::fprintf(stderr, "deferred WIT exit request failed: %s\n",
                   exit_smoke.lastError());
      return 1;
    }
    exit_smoke.shutdown();
  }
  std::printf("WAMR deferred exit request passed\n");
  {
    oos::runtime::NativeAppManager module_smoke(graphics, 1);
    oos::runtime::NativeAppLaunchOptions module_launch;
    module_launch.module_base_path = argv[7];
    module_launch.module_directory = argv[8];
    module_launch.font_directory = argv[6];
    module_launch.modules.push_back(
        {"echo", oos::apps::AppRuntimeKind::WebAssembly, "modules/echo"});
    module_launch.modules.push_back({"js-echo",
                                     oos::apps::AppRuntimeKind::JavaScript,
                                     "modules/js-echo.mjs"});
    uint32_t module_delay_ms = 0;
    if (!module_smoke.load("module-smoke", module_launch) ||
        !module_smoke.activate("module-smoke") ||
        !module_smoke.render(1'650'000, module_delay_ms) ||
        module_delay_ms != 1000) {
      std::fprintf(stderr, "Wasm package module smoke failed: %s\n",
                   module_smoke.lastError());
      return 1;
    }
    module_smoke.shutdown();
  }
  std::printf("Wasm-to-Wasm and Wasm-to-JS module invocation passed\n");
  {
    const std::filesystem::path js_root =
        std::filesystem::temp_directory_path() / "oos-js-to-wasm-smoke";
    std::filesystem::remove_all(js_root);
    std::filesystem::create_directories(js_root);
    const std::filesystem::path entry = js_root / "main.mjs";
    {
      std::ofstream output(entry);
      output << "let valid = false;\n"
                "export function initialize() {\n"
                "  const module = new WebAssembly.Module('echo');\n"
                "  const instance = new WebAssembly.Instance(module);\n"
                "  const response = instance.exports.echo(new Uint8Array([2, "
                "4, 6]));\n"
                "  valid = response.length === 3 && response[0] === 2 && "
                "response[2] === 6;\n"
                "  return valid;\n"
                "}\n"
                "export function frame() { return valid ? 1000 : 0; }\n";
    }
    oos::runtime::JsAppOptions js_options;
    js_options.app_id = "org.oos.js-to-wasm-smoke";
    js_options.application_directory = js_root.string();
    js_options.module_directory = argv[8];
    js_options.font_directory = argv[6];
    js_options.modules.push_back(
        {"echo", oos::apps::AppRuntimeKind::WebAssembly, "modules/echo"});
    oos::runtime::JsApp js_to_wasm(graphics, std::move(js_options));
    uint32_t delay = 0;
    if (!js_to_wasm.load(entry.c_str()) || !js_to_wasm.initialize() ||
        !js_to_wasm.render(1'660'000, delay) || delay != 1000) {
      std::fprintf(stderr, "JS-to-Wasm package module smoke failed: %s\n",
                   js_to_wasm.lastError());
      std::filesystem::remove_all(js_root);
      return 1;
    }
    js_to_wasm.shutdown();
    std::filesystem::remove_all(js_root);
  }
  std::printf("JS-to-Wasm WebAssembly facade invocation passed\n");
  const auto runWasmUiDemo = [&](const char *id, const char *base_path,
                                 uint32_t expected_delay) {
    oos::runtime::NativeAppManager demo(graphics, 1);
    oos::runtime::NativeAppLaunchOptions options;
    options.module_base_path = base_path;
    options.font_directory = argv[6];
    uint32_t delay = 0;
    if (!demo.load(id, options) || !demo.activate(id) ||
        !demo.render(1'670'000, delay) || delay != expected_delay) {
      std::fprintf(stderr, "%s framework demo failed: %s\n", id,
                   demo.lastError());
      return false;
    }
    oos::input::KeyEvent event;
    event.code = 352;
    event.action = oos::input::KeyAction::Pressed;
    if (!demo.dispatchKey(event, 1'680'000) || !demo.render(1'690'000, delay)) {
      std::fprintf(stderr, "%s framework demo input failed: %s\n", id,
                   demo.lastError());
      return false;
    }
    demo.shutdown();
    return true;
  };
  if (!runWasmUiDemo("lvgl-demo", argv[10], 16) ||
      !runWasmUiDemo("clay-demo", argv[11], 1000))
    return 1;
  std::printf("LVGL and Clay shared graphics backend demos passed\n");
  {
    const std::filesystem::path solid_entry =
        std::filesystem::canonical(argv[12]);
    oos::runtime::JsAppOptions options;
    options.app_id = "cc.jaxy.oos.solid-demo";
    options.application_directory = solid_entry.parent_path().string();
    options.module_directory = options.application_directory;
    options.font_directory = argv[6];
    oos::runtime::JsApp solid(graphics, std::move(options));
    uint32_t delay = 0;
    oos::input::KeyEvent event;
    event.code = 352;
    event.action = oos::input::KeyAction::Pressed;
    if (!solid.load(solid_entry.c_str()) || !solid.initialize() ||
        !solid.render(1'700'000, delay) || delay != 1000 ||
        !solid.dispatchKey(event, 1'710'000) ||
        !solid.render(1'720'000, delay)) {
      std::fprintf(stderr, "Solid framework demo failed: %s\n",
                   solid.lastError());
      return 1;
    }
    solid.shutdown();
  }
  std::printf("Solid universal renderer demo passed\n");
  oos::runtime::NativeAppManager mock_smoke(graphics, mock_device, 1);
  char storage_template[] = "/tmp/oos-wasm-storage.XXXXXX";
  const char *storage_root = mkdtemp(storage_template);
  if (!storage_root) {
    std::perror("mkdtemp");
    return 1;
  }
  oos::runtime::NativeAppLaunchOptions mock_launch;
  mock_launch.module_base_path = argv[2];
  mock_launch.data_directory = storage_root;
  mock_launch.system_data_root = storage_root;
  mock_launch.font_directory = argv[6];
  mock_launch.status_bar = &status_bar;
  const std::string internal_media = std::string(storage_root) + "/internal";
  const std::string removable_media = std::string(storage_root) + "/removable";
  const std::string packaged_assets = std::string(storage_root) + "/assets";
  if (!std::filesystem::create_directories(internal_media) ||
      !std::filesystem::create_directories(removable_media) ||
      !std::filesystem::create_directories(packaged_assets)) {
    std::fprintf(stderr, "cannot create WIT media test roots\n");
    return 1;
  }
  mock_launch.internal_media_directory = internal_media.c_str();
  mock_launch.removable_media_directory = removable_media.c_str();
  const std::string test_asset = packaged_assets + "/test.dat";
  {
    std::ofstream output(test_asset, std::ios::binary);
    output << "test-asset";
  }
  mock_launch.asset_directory = packaged_assets.c_str();
  const std::vector<std::string> mock_permissions = {
      "audio-capture",
      "camera",
      "power",
      "wifi-manage",
      "bluetooth",
      "mobileconnection",
      "mobileconnection:identity",
      "mobileconnection:radio-control",
      "device-storage:read",
      "device-storage:write",
      "device-storage:create",
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
  MockDevice read_only_device("local-storage-readonly");
  oos::runtime::NativeAppManager read_only_smoke(graphics, read_only_device, 1);
  oos::runtime::NativeAppLaunchOptions read_only_launch = mock_launch;
  read_only_launch.service_permission_mask = oos::apps::permissionBit(
      oos::apps::DeviceServicePermission::DeviceStorageRead);
  if (!read_only_smoke.load("storage-readonly", read_only_launch) ||
      !read_only_smoke.activate("storage-readonly") ||
      !read_only_smoke.render(1'800'000)) {
    std::fprintf(stderr, "WIT read-only device storage smoke failed: %s\n",
                 read_only_smoke.lastError());
    return 1;
  }
  read_only_smoke.shutdown();
  std::filesystem::remove_all(storage_root);
  std::printf("WAMR WIT local mock and permission filtering passed\n");
  const bool valid =
      status_bar.updates == 4 &&
      status_bar.appearance == (oos::ui::StatusBarAppearance{0x123456, true}) &&
      graphics.frames >= 5 && resident_textures == 3 &&
      graphics.textures.empty() && graphics.texture_updates > 0 &&
      graphics.gles_frames >= 4;
  if (!valid) {
    std::fprintf(stderr,
                 "host invariants failed: status=%zu frames=%zu resident=%zu "
                 "textures=%zu updates=%zu gles=%zu\n",
                 status_bar.updates, graphics.frames, resident_textures,
                 graphics.textures.size(), graphics.texture_updates,
                 graphics.gles_frames);
  }
  return valid ? 0 : 1;
}
