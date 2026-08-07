#include <png.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "oos/apps/app_repository.h"
#include "oos/apps/launcher/launcher.h"
#include "oos/apps/permissions.h"
#include "oos/apps/settings/settings.h"
#include "oos/apps/systemui/system_ui.h"
#include "oos/compositor/compositor.h"
#include "oos/device/device.h"
#include "oos/device/display.h"
#include "oos/input/key_input.h"
#include "oos/resources/boot_splash.h"
#include "oos/runtime/application_scene.h"
#include "oos/runtime/application_session_manager.h"
#include "oos/runtime/js_app.h"
#include "oos/runtime/wasm_app.h"
#include "oos/sdk/ui/theme.h"
#include "oos/ui/system_status.h"
#include "oos/ui/system_ui_settings.h"
#include "oos/window/input_router.h"

namespace oos::platform {
namespace {

using oos::compositor::Compositor;
using oos::device::Device;
using oos::device::Display;
using oos::input::KeyEvent;
using oos::input::KeyInputSource;
using oos::runtime::ApplicationSession;
using oos::runtime::ApplicationSessionManager;
using oos::runtime::GraphicsHost;
using oos::runtime::JsApp;
using oos::runtime::JsAppOptions;
using oos::runtime::WasmApp;
using oos::runtime::WasmAppOptions;

constexpr int kFrameIntervalMs = 33;
constexpr const char *kLauncherId = "cc.jaxy.oos.launcher";
constexpr const char *kSettingsId = "cc.jaxy.oos.settings";

class LauncherSession final : public ApplicationSession {
public:
  LauncherSession(GraphicsHost &graphics, oos::apps::AppRepository &repository,
                  oos::ui::StatusBarAppearanceController &status_bar)
      : app_(graphics, repository, status_bar) {}

  bool dispatchKey(const KeyEvent &event, int64_t monotonic_us) override {
    return app_.dispatchKey(event, monotonic_us);
  }
  bool initialize() override { return app_.initialize(); }
  void shutdown() override { app_.shutdown(); }
  bool frame(int64_t monotonic_us, uint32_t &next_delay_ms) override {
    return app_.frame(monotonic_us, next_delay_ms);
  }
  std::string takeLaunchRequest() override { return app_.takeLaunchRequest(); }
  bool takeExitRequest() override { return false; }
  const char *lastError() const override { return app_.lastError(); }

private:
  oos::apps::launcher::Launcher app_;
};

class SettingsSession final : public ApplicationSession {
public:
  SettingsSession(GraphicsHost &graphics, oos::apps::AppRepository &repository,
                  const oos::device::Device &device,
                  oos::ui::SystemUiSettings &system,
                  oos::ui::StatusBarAppearanceController &status_bar,
                  std::string data_root)
      : app_(graphics, repository, device, system, status_bar,
             std::move(data_root)) {}

  bool dispatchKey(const KeyEvent &event, int64_t monotonic_us) override {
    return app_.dispatchKey(event, monotonic_us);
  }
  bool initialize() override { return app_.initialize(); }
  void shutdown() override { app_.shutdown(); }
  bool frame(int64_t monotonic_us, uint32_t &next_delay_ms) override {
    return app_.frame(monotonic_us, next_delay_ms);
  }
  std::string takeLaunchRequest() override { return app_.takeLaunchRequest(); }
  bool takeExitRequest() override { return false; }
  const char *lastError() const override { return app_.lastError(); }

private:
  oos::apps::settings::Settings app_;
};

struct PackagedSessionConfig {
  oos::apps::AppRuntimeKind runtime = oos::apps::AppRuntimeKind::WebAssembly;
  std::string executable_path;
  oos::runtime::ApplicationContextOptions context;
  std::string application_directory;
  std::string module_directory;
  std::vector<oos::apps::AppModule> modules;
};

class PackagedSession final : public ApplicationSession {
public:
  PackagedSession(GraphicsHost &graphics, oos::device::Device &device,
                  oos::ui::StatusBarAppearanceController &status_bar,
                  PackagedSessionConfig config)
      : executable_path_(std::move(config.executable_path)),
        scene_(graphics, config.context.font_directory),
        runtime_(config.runtime) {
    config.context.status_bar = &status_bar;
    if (runtime_ == oos::apps::AppRuntimeKind::JavaScript) {
      JsAppOptions options;
      static_cast<oos::runtime::ApplicationContextOptions &>(options) =
          std::move(config.context);
      options.application_directory = std::move(config.application_directory);
      options.module_directory = std::move(config.module_directory);
      options.modules = std::move(config.modules);
      js_ = std::make_unique<JsApp>(scene_, device, std::move(options));
    } else {
      WasmAppOptions options;
      static_cast<oos::runtime::ApplicationContextOptions &>(options) =
          std::move(config.context);
      options.module_directory = std::move(config.module_directory);
      options.modules = std::move(config.modules);
      wasm_ = std::make_unique<WasmApp>(scene_, device, std::move(options));
    }
  }

  bool initialize() override {
    return js_ ? js_->load(executable_path_.c_str()) && js_->initialize()
               : wasm_->load(executable_path_.c_str()) && wasm_->initialize();
  }
  void shutdown() override {
    if (js_)
      js_->shutdown();
    else
      wasm_->shutdown();
  }
  void onActivated() override { setAudioFocused(true); }
  void onDeactivated() override { setAudioFocused(false); }
  bool dispatchKey(const KeyEvent &event, int64_t monotonic_us) override {
    return js_ ? js_->dispatchKey(event, monotonic_us)
               : wasm_->dispatchKey(event, monotonic_us);
  }
  bool frame(int64_t monotonic_us, uint32_t &next_delay_ms) override {
    const bool rendered = js_ ? js_->render(monotonic_us, next_delay_ms)
                              : wasm_->render(monotonic_us, next_delay_ms);
    return rendered && scene_.present();
  }
  std::string takeLaunchRequest() override { return {}; }
  bool takeExitRequest() override {
    return js_ ? js_->takeExitRequest() : wasm_->takeExitRequest();
  }
  const char *lastError() const override {
    return js_ ? js_->lastError() : wasm_->lastError();
  }

private:
  void setAudioFocused(bool focused) {
    if (js_)
      js_->setAudioFocused(focused);
    else
      wasm_->setAudioFocused(focused);
  }

  std::string executable_path_;
  oos::runtime::ApplicationScene scene_;
  oos::apps::AppRuntimeKind runtime_;
  std::unique_ptr<JsApp> js_;
  std::unique_ptr<WasmApp> wasm_;
};

volatile std::sig_atomic_t g_stop_requested = 0;

const char *environmentOr(const char *name, const char *fallback) {
  const char *value = std::getenv(name);
  return value && value[0] != '\0' ? value : fallback;
}

bool validAppId(const char *app_id) {
  if (!app_id || !app_id[0])
    return false;
  for (const unsigned char character : std::string(app_id)) {
    if ((character < 'a' || character > 'z') &&
        (character < 'A' || character > 'Z') &&
        (character < '0' || character > '9') && character != '.' &&
        character != '_' && character != '-')
      return false;
  }
  return true;
}

bool importBundledApp(oos::apps::AppRepository &repository,
                      const char *app_id) {
  if (!validAppId(app_id))
    return false;
  std::string package_path;
  package_path = environmentOr("OOS_PACKAGE_ROOT", "/opt/oos/packages");
  package_path += "/";
  package_path += app_id;
  package_path += "/application.zip";
  oos::apps::AppInstallOptions options;
  options.app_id = app_id;
  return repository.install(package_path.c_str(), options);
}

bool preparePackagedSession(oos::apps::AppRepository &repository,
                            const std::string &app_id, const char *data_root,
                            PackagedSessionConfig &config) {
  oos::apps::AppRecord record;
  if (!repository.resolve(app_id.c_str(), record) &&
      !importBundledApp(repository, app_id.c_str()))
    return false;
  oos::apps::AppLaunch launch;
  if (!repository.prepareLaunch(app_id.c_str(), launch))
    return false;
  config = {};
  config.runtime = launch.app.manifest.entry.runtime;
  config.executable_path = launch.executable_path;
  config.application_directory = launch.application_directory;
  config.module_directory = launch.module_directory;
  config.modules = launch.app.manifest.modules;
  config.context.app_id = app_id;
  config.context.data_directory = launch.data_directory;
  config.context.system_data_root = data_root;
  config.context.app_repository = &repository;
  config.context.asset_directory = launch.asset_directory;
  config.context.service_permission_mask =
      oos::apps::deviceServicePermissionMask(
          launch.app.manifest.requested_permissions);
  config.context.enforce_service_permissions = true;
  return true;
}

int waitForShellEvents(KeyInputSource &input, int wake_fd, int timeout_ms,
                       oos::input::KeyEventCallback callback, void *context) {
  const int input_fd = input.fileDescriptor();
  if (input_fd < 0)
    return input.poll(timeout_ms, callback, context);
  {
    pollfd descriptors[2] = {{input_fd, POLLIN, 0}, {wake_fd, POLLIN, 0}};
    const nfds_t count = wake_fd >= 0 ? 2 : 1;
    int result;
    do {
      result = ::poll(descriptors, count, timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
      return -1;
  }
  if (wake_fd >= 0) {
    uint64_t pending = 0;
    while (::read(wake_fd, &pending, sizeof(pending)) < 0 && errno == EINTR) {
    }
  }
  return input.poll(0, callback, context);
}

int64_t monotonicMicros() {
  timespec time = {};
  clock_gettime(CLOCK_MONOTONIC, &time);
  return static_cast<int64_t>(time.tv_sec) * 1000000 + time.tv_nsec / 1000;
}

void stopRuntime(int) { g_stop_requested = 1; }

bool loadBootSplash(uint32_t expected_width, uint32_t expected_height,
                    std::vector<uint16_t> &rgb565) {
  png_image image = {};
  image.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(
          &image, oos::resources::embeddedBootSplashPng(),
          oos::resources::embeddedBootSplashPngSize())) {
    std::fprintf(stderr, "failed to decode embedded boot splash: %s\n",
                 image.message);
    return false;
  }
  if (image.width != expected_width || image.height != expected_height) {
    std::fprintf(stderr, "boot splash has invalid size %ux%u, expected %ux%u\n",
                 image.width, image.height, expected_width, expected_height);
    png_image_free(&image);
    return false;
  }
  image.format = PNG_FORMAT_RGB;
  std::vector<uint8_t> rgb(PNG_IMAGE_SIZE(image));
  if (!png_image_finish_read(&image, nullptr, rgb.data(), 0, nullptr)) {
    std::fprintf(stderr, "failed to decode embedded boot splash: %s\n",
                 image.message);
    png_image_free(&image);
    return false;
  }
  png_image_free(&image);
  rgb565.resize(static_cast<size_t>(expected_width) * expected_height);
  for (size_t index = 0; index < rgb565.size(); ++index) {
    const uint8_t red = rgb[index * 3];
    const uint8_t green = rgb[index * 3 + 1];
    const uint8_t blue = rgb[index * 3 + 2];
    rgb565[index] = static_cast<uint16_t>(((red & 0xf8) << 8) |
                                          ((green & 0xfc) << 3) | (blue >> 3));
  }
  return true;
}

struct ShellInputContext {
  oos::window::InputRouter *router;
  bool success = true;
};

void dispatchShellKey(void *data, const KeyEvent &event) {
  auto *context = static_cast<ShellInputContext *>(data);
  if (!context->router->dispatch(event, monotonicMicros())) {
    std::fprintf(stderr, "input routing failed: %s\n",
                 context->router->lastError().c_str());
    context->success = false;
  }
}

void printUsage(const char *program) {
  std::fprintf(stderr,
               "usage: %s [--builtin launcher|settings | --app APP_ID | "
               "--module WASM_BASE]\n"
               "       %s --install APPLICATION.zip\n"
               "       %s --list-apps\n",
               program, program, program);
}

int runRepositoryCommand(int argc, char **argv,
                         oos::apps::AppRepository &repository) {
  if (argc == 3 && std::strcmp(argv[1], "--install") == 0) {
    oos::apps::AppRecord installed;
    if (!repository.install(argv[2], &installed)) {
      std::fprintf(stderr, "install failed: %s\n",
                   repository.lastError().c_str());
      return 1;
    }
    std::printf("%s\t%s\n", installed.manifest.id.c_str(),
                installed.manifest.version.c_str());
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--list-apps") == 0) {
    std::vector<oos::apps::AppRecord> records;
    if (!repository.list(records)) {
      std::fprintf(stderr, "list applications failed: %s\n",
                   repository.lastError().c_str());
      return 1;
    }
    for (const auto &record : records) {
      std::printf("%s\t%s\t%s\n", record.manifest.id.c_str(),
                  record.manifest.version.c_str(),
                  record.enabled ? "enabled" : "disabled");
    }
    return 0;
  }
  return -1;
}

} // namespace

int run(int argc, char **argv) {
  if (argc > 3) {
    printUsage(argv[0]);
    return 2;
  }

  const char *data_root = environmentOr("OOS_DATA_ROOT", "/data");
  oos::apps::AppRepository repository(data_root);
  if (!repository.initialize()) {
    std::fprintf(stderr, "initialize application repository failed: %s\n",
                 repository.lastError().c_str());
    return 1;
  }
  const int repository_result = runRepositoryCommand(argc, argv, repository);
  if (repository_result >= 0)
    return repository_result;

  const char *builtin_id = argc == 1 ? kLauncherId : nullptr;
  const char *raw_module = nullptr;
  const char *app_id = nullptr;
  if (argc == 2) {
    raw_module = argv[1];
  } else if (argc == 3 && std::strcmp(argv[1], "--module") == 0) {
    raw_module = argv[2];
  } else if (argc == 3 && std::strcmp(argv[1], "--app") == 0) {
    app_id = argv[2];
  } else if (argc == 3 && std::strcmp(argv[1], "--builtin") == 0) {
    if (std::strcmp(argv[2], "launcher") == 0)
      builtin_id = kLauncherId;
    else if (std::strcmp(argv[2], "settings") == 0)
      builtin_id = kSettingsId;
    else {
      printUsage(argv[0]);
      return 2;
    }
  } else if (argc != 1) {
    printUsage(argv[0]);
    return 2;
  }

  PackagedSessionConfig initial_app;
  if (builtin_id) {
    // Built-in shell applications are separate targets managed by the host.
  } else if (raw_module) {
    initial_app.runtime = oos::apps::AppRuntimeKind::WebAssembly;
    initial_app.executable_path = raw_module;
    initial_app.context.app_id = "diagnostic";
    initial_app.context.system_data_root = data_root;
    initial_app.context.app_repository = &repository;
  } else {
    if (!preparePackagedSession(repository, app_id, data_root, initial_app)) {
      std::fprintf(stderr, "prepare application %s failed: %s\n", app_id,
                   repository.lastError().c_str());
      return 1;
    }
  }

  std::unique_ptr<Device> platform_device = device::createDevice();
  if (!platform_device || !platform_device->initialize()) {
    std::fprintf(stderr, "failed to initialize OOS device%s%s\n",
                 platform_device ? ": " : "",
                 platform_device ? platform_device->lastError().c_str()
                                 : "factory unavailable");
    return 1;
  }
  Display &display = platform_device->display();
  KeyInputSource &input = platform_device->keyInput();
  const device::DeviceDescriptor &descriptor = platform_device->descriptor();

  std::vector<uint16_t> boot_frame;
  if (!loadBootSplash(descriptor.primary_width, descriptor.primary_height,
                      boot_frame) ||
      !display.showBootFrame(boot_frame.data())) {
    std::fprintf(stderr, "failed to present OOS boot splash\n");
    platform_device->shutdown();
    return 1;
  }

  Compositor compositor(display);
  std::signal(SIGINT, stopRuntime);
  std::signal(SIGTERM, stopRuntime);

  constexpr uint32_t kStatusHeight = 22;
  const uint32_t content_height = descriptor.primary_height - kStatusHeight;
  auto overlay_surface = compositor.createLayer(
      {"system-overlay", 0, static_cast<int32_t>(kStatusHeight),
       descriptor.primary_width, content_height, 100});
  auto status_surface = compositor.createLayer(
      {"status-bar", 0, 0, descriptor.primary_width, kStatusHeight, 200});
  if (!overlay_surface || !status_surface) {
    std::fprintf(stderr, "failed to create OOS compositor layers\n");
    platform_device->shutdown();
    return 1;
  }

  oos::ui::DeviceStatusMonitor status_monitor(*platform_device);
  status_monitor.start();
  oos::ui::SystemUiSettings shell_settings(data_root);
  if (!shell_settings.initialize())
    std::fprintf(stderr, "load system UI settings failed: %s\n",
                 shell_settings.lastError().c_str());
  oos::apps::systemui::SystemUi system_ui(*status_surface, *overlay_surface,
                                          &status_monitor, &shell_settings);
  if (!system_ui.initialize()) {
    std::fprintf(stderr, "failed to start SystemUI: %s\n",
                 system_ui.lastError().c_str());
    status_monitor.stop();
    platform_device->shutdown();
    return 1;
  }

  const int wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd < 0)
    std::fprintf(stderr, "create runtime wake event failed: %s\n",
                 std::strerror(errno));

  ApplicationSessionManager sessions(
      compositor, 0, static_cast<int32_t>(kStatusHeight),
      descriptor.primary_width, content_height, system_ui,
      {oos::sdk::ui::theme::kStatusBackground, false});
  if (!sessions.registerFactory(
          kLauncherId,
          [&](GraphicsHost &graphics,
              oos::ui::StatusBarAppearanceController &status_bar) {
            return std::make_unique<LauncherSession>(graphics, repository,
                                                     status_bar);
          }) ||
      !sessions.registerFactory(
          kSettingsId, [&](GraphicsHost &graphics,
                           oos::ui::StatusBarAppearanceController &status_bar) {
            return std::make_unique<SettingsSession>(
                graphics, repository, *platform_device, shell_settings,
                status_bar, data_root);
          })) {
    std::fprintf(stderr, "register built-in applications failed: %s\n",
                 sessions.lastError());
    system_ui.shutdown();
    status_monitor.stop();
    platform_device->shutdown();
    return 1;
  }

  auto register_app = [&](const std::string &id,
                          const PackagedSessionConfig &config) {
    PackagedSessionConfig launch_config = config;
    launch_config.context.wake_fd = wake_fd;
    return sessions.registerFactory(
        id,
        [&, launch_config](GraphicsHost &graphics,
                           oos::ui::StatusBarAppearanceController &status_bar) {
          return std::make_unique<PackagedSession>(graphics, *platform_device,
                                                   status_bar, launch_config);
        });
  };
  auto ensure_registered = [&](const std::string &id) {
    if (sessions.registered(id))
      return true;
    PackagedSessionConfig config;
    if (!preparePackagedSession(repository, id, data_root, config))
      return false;
    return register_app(id, config);
  };

  std::string runtime_id = builtin_id   ? builtin_id
                           : raw_module ? "diagnostic"
                                        : app_id;
  if (!builtin_id && !register_app(runtime_id, initial_app)) {
    std::fprintf(stderr, "register native application %s failed: %s\n",
                 runtime_id.c_str(), sessions.lastError());
    system_ui.shutdown();
    status_monitor.stop();
    platform_device->shutdown();
    return 1;
  }
  if (!sessions.activate(runtime_id)) {
    std::fprintf(stderr, "start application %s failed: %s\n",
                 runtime_id.c_str(), sessions.lastError());
    sessions.shutdown();
    system_ui.shutdown();
    status_monitor.stop();
    platform_device->shutdown();
    return 1;
  }
  std::fprintf(stderr, "OOS shell started on %s: app=%s\n", descriptor.id,
               runtime_id.c_str());
  std::fflush(stderr);

  oos::window::InputRouter input_router(system_ui, sessions);
  ShellInputContext input_context{&input_router};
  while (!g_stop_requested && !input.stopRequested()) {
    const std::string launch_request = sessions.takeLaunchRequest();
    if (!launch_request.empty()) {
      if (!ensure_registered(launch_request) ||
          !sessions.activate(launch_request)) {
        const char *message = sessions.lastError()[0]
                                  ? sessions.lastError()
                                  : repository.lastError().c_str();
        std::fprintf(stderr, "switch to application %s failed: %s\n",
                     launch_request.c_str(), message);
        break;
      }
      runtime_id = launch_request;
    }
    if (sessions.takeExitRequest()) {
      const std::string closing_id = runtime_id;
      if (closing_id == kLauncherId || !sessions.activate(kLauncherId) ||
          !sessions.destroy(closing_id)) {
        std::fprintf(stderr, "close application %s failed: %s\n",
                     closing_id.c_str(), sessions.lastError());
        break;
      }
      runtime_id = kLauncherId;
    }
    const int64_t frame_time = monotonicMicros();
    uint32_t system_ui_delay_ms = 1000;
    uint32_t application_delay_ms = 1000;
    if (!system_ui.frame(frame_time, system_ui_delay_ms)) {
      std::fprintf(stderr, "SystemUI frame failed: %s\n",
                   system_ui.lastError().c_str());
      break;
    }
    if (!system_ui.locked()) {
      if (!sessions.frame(frame_time, application_delay_ms)) {
        std::fprintf(stderr, "application frame failed: %s\n",
                     sessions.lastError());
        break;
      }
    }
    if (compositor.dirty() && !compositor.compose()) {
      std::fprintf(stderr, "compositor frame failed\n");
      break;
    }
    const uint32_t requested_delay =
        std::min(system_ui_delay_ms, application_delay_ms);
    const int timeout_ms = static_cast<int>(
        std::min<uint32_t>(requested_delay, static_cast<uint32_t>(1000)));
    if (waitForShellEvents(input, wake_fd, timeout_ms, dispatchShellKey,
                           &input_context) < 0 ||
        !input_context.success)
      break;
  }

  const bool stopped = g_stop_requested || input.stopRequested();
  sessions.shutdown();
  system_ui.shutdown();
  status_monitor.stop();
  platform_device->shutdown();
  if (wake_fd >= 0)
    ::close(wake_fd);
  return stopped ? 0 : 1;
}

} // namespace oos::platform
