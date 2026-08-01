#include <png.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "oos/apps/app_repository.h"
#include "oos/apps/launcher/launcher.h"
#include "oos/apps/permissions.h"
#include "oos/apps/systemui/system_ui.h"
#include "oos/compositor/compositor.h"
#include "oos/device/device.h"
#include "oos/device/display.h"
#include "oos/input/key_input.h"
#include "oos/resources/boot_splash.h"
#include "oos/runtime/native_app_manager.h"
#include "oos/ui/system_status.h"
#include "oos/window/input_router.h"

namespace oos::platform {
namespace {

using oos::compositor::Compositor;
using oos::device::Device;
using oos::device::Display;
using oos::input::KeyEvent;
using oos::input::KeyInputSource;
using oos::runtime::NativeAppLaunchOptions;
using oos::runtime::NativeAppManager;

constexpr int kFrameIntervalMs = 33;

class NativeAppInputTarget final : public oos::window::ApplicationInputTarget {
public:
  explicit NativeAppInputTarget(NativeAppManager &apps) : apps_(apps) {}

  bool dispatchKey(const KeyEvent &event, int64_t monotonic_us) override {
    return apps_.dispatchKey(event, monotonic_us);
  }
  const char *lastError() const override { return apps_.lastError(); }

private:
  NativeAppManager &apps_;
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
               "usage: %s [--app APP_ID | --module APP.wasm|APP.aot]\n"
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

  const char *raw_module = nullptr;
  const char *app_id = nullptr;
  const bool run_launcher = argc == 1;
  if (argc == 2) {
    // Keep the original positional module form for existing device diagnostics.
    raw_module = argv[1];
  } else if (argc == 3 && std::strcmp(argv[1], "--module") == 0) {
    raw_module = argv[2];
  } else if (argc == 3 && std::strcmp(argv[1], "--app") == 0) {
    app_id = argv[2];
  } else if (argc != 1) {
    printUsage(argv[0]);
    return 2;
  }

  oos::apps::AppLaunch app_launch;
  NativeAppLaunchOptions native_launch;
  if (run_launcher) {
    // The built-in LVGL Launcher is a separate application target, not part
    // of SystemUI and not dispatched through WAMR.
  } else if (raw_module) {
    native_launch.module_path = raw_module;
  } else {
    if (!repository.resolve(app_id, app_launch.app)) {
      if (!importBundledApp(repository, app_id)) {
        std::fprintf(stderr, "import bundled application %s failed: %s\n",
                     app_id, repository.lastError().c_str());
        return 1;
      }
    }
    if (!repository.prepareLaunch(app_id, app_launch)) {
      std::fprintf(stderr, "prepare application %s failed: %s\n", app_id,
                   repository.lastError().c_str());
      return 1;
    }
    native_launch.module_path = app_launch.executable_path.c_str();
    native_launch.data_directory = app_launch.data_directory.c_str();
    native_launch.system_data_root = data_root;
    native_launch.app_repository = &repository;
    native_launch.service_permission_mask =
        oos::apps::deviceServicePermissionMask(
            app_launch.app.manifest.requested_permissions);
    native_launch.enforce_service_permissions = true;
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
  auto app_surface = compositor.createLayer(
      {"application", 0, static_cast<int32_t>(kStatusHeight),
       descriptor.primary_width, content_height, 0});
  auto overlay_surface = compositor.createLayer(
      {"system-overlay", 0, static_cast<int32_t>(kStatusHeight),
       descriptor.primary_width, content_height, 100});
  auto status_surface = compositor.createLayer(
      {"status-bar", 0, 0, descriptor.primary_width, kStatusHeight, 200});
  if (!app_surface || !overlay_surface || !status_surface) {
    std::fprintf(stderr, "failed to create OOS compositor layers\n");
    platform_device->shutdown();
    return 1;
  }

  oos::ui::DeviceStatusMonitor status_monitor(*platform_device);
  status_monitor.start();
  oos::apps::systemui::SystemUi system_ui(*status_surface, *overlay_surface,
                                          &status_monitor);
  if (!system_ui.initialize()) {
    std::fprintf(stderr, "failed to start SystemUI: %s\n",
                 system_ui.lastError().c_str());
    status_monitor.stop();
    platform_device->shutdown();
    return 1;
  }

  std::unique_ptr<oos::apps::launcher::Launcher> launcher;
  std::unique_ptr<NativeAppManager> apps;
  std::unique_ptr<NativeAppInputTarget> native_input;
  oos::window::ApplicationInputTarget *foreground_input = nullptr;
  const char *runtime_id = run_launcher ? "cc.jaxy.oos.launcher"
                           : raw_module ? "diagnostic"
                                        : app_id;
  if (run_launcher) {
    launcher = std::make_unique<oos::apps::launcher::Launcher>(*app_surface,
                                                               repository);
    if (!launcher->initialize()) {
      std::fprintf(stderr, "failed to start Launcher: %s\n",
                   launcher->lastError());
      system_ui.shutdown();
      status_monitor.stop();
      platform_device->shutdown();
      return 1;
    }
    foreground_input = launcher.get();
  } else {
    apps = std::make_unique<NativeAppManager>(*app_surface, *platform_device);
    if (!apps->load(runtime_id, native_launch) || !apps->activate(runtime_id)) {
      std::fprintf(stderr, "failed to start native app %s: %s\n",
                   native_launch.module_path, apps->lastError());
      system_ui.shutdown();
      status_monitor.stop();
      platform_device->shutdown();
      return 1;
    }
    native_input = std::make_unique<NativeAppInputTarget>(*apps);
    foreground_input = native_input.get();
  }
  std::fprintf(stderr, "OOS shell started on %s: app=%s\n", descriptor.id,
               runtime_id);
  std::fflush(stderr);

  oos::window::InputRouter input_router(system_ui, *foreground_input);
  ShellInputContext input_context{&input_router};
  auto next_frame = std::chrono::steady_clock::now();
  while (!g_stop_requested && !input.stopRequested()) {
    if (input.poll(0, dispatchShellKey, &input_context) < 0 ||
        !input_context.success) {
      break;
    }
    const int64_t frame_time = monotonicMicros();
    uint32_t system_ui_delay_ms = 1000;
    if (!system_ui.frame(frame_time, system_ui_delay_ms)) {
      std::fprintf(stderr, "SystemUI frame failed: %s\n",
                   system_ui.lastError().c_str());
      break;
    }
    if (!system_ui.locked()) {
      if (launcher) {
        uint32_t launcher_delay_ms = 1000;
        if (!launcher->frame(frame_time, launcher_delay_ms)) {
          std::fprintf(stderr, "Launcher frame failed: %s\n",
                       launcher->lastError());
          break;
        }
      } else if (!apps->render(frame_time)) {
        std::fprintf(stderr, "WASM frame failed: %s\n", apps->lastError());
        break;
      }
    }
    if (compositor.dirty() && !compositor.compose()) {
      std::fprintf(stderr, "compositor frame failed\n");
      break;
    }
    next_frame += std::chrono::milliseconds(kFrameIntervalMs);
    const auto now = std::chrono::steady_clock::now();
    if (next_frame <= now)
      next_frame = now;
    else
      std::this_thread::sleep_until(next_frame);
  }

  const bool stopped = g_stop_requested || input.stopRequested();
  if (apps)
    apps->shutdown();
  if (launcher)
    launcher->shutdown();
  system_ui.shutdown();
  status_monitor.stop();
  platform_device->shutdown();
  return stopped ? 0 : 1;
}

} // namespace oos::platform
