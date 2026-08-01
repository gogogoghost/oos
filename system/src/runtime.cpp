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
#include "oos/apps/permissions.h"
#include "oos/compositor/compositor.h"
#include "oos/device/device.h"
#include "oos/device/display.h"
#include "oos/input/key_input.h"
#include "oos/resources/boot_splash.h"
#include "oos/runtime/native_app_manager.h"
#include "oos/ui/system_ui.h"

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

struct NativeInputContext {
  NativeAppManager *apps;
  bool success = true;
};

void dispatchNativeKey(void *data, const KeyEvent &event) {
  auto *context = static_cast<NativeInputContext *>(data);
  if (!context->apps->dispatchKey(event, monotonicMicros())) {
    std::fprintf(stderr, "WASM key dispatch failed: %s\n",
                 context->apps->lastError());
    context->success = false;
  }
}

struct SystemUiInputContext {
  oos::ui::SystemUi *system_ui;
  bool success = true;
};

void dispatchSystemUiKey(void *data, const KeyEvent &event) {
  auto *context = static_cast<SystemUiInputContext *>(data);
  if (!context->system_ui->dispatchKey(event))
    context->success = false;
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
    std::printf("%s\t%s\t%s\n", installed.manifest.id.c_str(),
                installed.manifest.version.c_str(),
                oos::apps::runtimeKindName(installed.manifest.runtime_kind));
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
      std::printf("%s\t%s\t%s\t%s\n", record.manifest.id.c_str(),
                  record.manifest.version.c_str(),
                  oos::apps::runtimeKindName(record.manifest.runtime_kind),
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

  const bool run_system_ui = argc == 1;
  const char *raw_module = nullptr;
  const char *app_id = nullptr;
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
  if (!run_system_ui && raw_module) {
    native_launch.module_path = raw_module;
  } else if (!run_system_ui) {
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
    native_launch.stack_size = app_launch.app.manifest.stack_bytes;
    native_launch.heap_size = app_launch.app.manifest.heap_bytes;
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

  if (run_system_ui) {
    oos::ui::SystemUi system_ui(compositor, repository);
    if (!system_ui.initialize()) {
      std::fprintf(stderr, "failed to start SystemUI: %s\n",
                   system_ui.lastError().c_str());
      platform_device->shutdown();
      return 1;
    }
    std::fprintf(stderr, "OOS LVGL SystemUI started on %s\n", descriptor.id);
    std::fflush(stderr);
    SystemUiInputContext input_context{&system_ui};
    uint32_t next_delay_ms = 1;
    while (!g_stop_requested && !input.stopRequested()) {
      if (!system_ui.frame(monotonicMicros(), next_delay_ms)) {
        std::fprintf(stderr, "SystemUI frame failed: %s\n",
                     system_ui.lastError().c_str());
        break;
      }
      const int poll_result =
          input.poll(static_cast<int>(next_delay_ms), dispatchSystemUiKey,
                     &input_context);
      if (poll_result < 0 || !input_context.success)
        break;
    }
    const bool stopped = g_stop_requested || input.stopRequested();
    system_ui.shutdown();
    platform_device->shutdown();
    return stopped ? 0 : 1;
  }

  NativeAppManager apps(compositor, *platform_device);
  const char *runtime_id = raw_module ? "diagnostic" : app_id;
  if (!apps.load(runtime_id, native_launch) || !apps.activate(runtime_id)) {
    std::fprintf(stderr, "failed to start native app %s: %s\n",
                 native_launch.module_path, apps.lastError());
    platform_device->shutdown();
    return 1;
  }
  std::fprintf(stderr, "OOS native app started on %s: %s\n", descriptor.id,
               native_launch.module_path);
  std::fflush(stderr);

  NativeInputContext input_context{&apps};
  auto next_frame = std::chrono::steady_clock::now();
  while (!g_stop_requested && !input.stopRequested()) {
    if (input.poll(0, dispatchNativeKey, &input_context) < 0 ||
        !input_context.success) {
      break;
    }
    if (!apps.render(monotonicMicros())) {
      std::fprintf(stderr, "WASM frame failed: %s\n", apps.lastError());
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
  apps.shutdown();
  platform_device->shutdown();
  return stopped ? 0 : 1;
}

} // namespace oos::platform
