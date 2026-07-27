#include <png.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "oos/compositor/compositor.h"
#include "oos/device/device.h"
#include "oos/device/display.h"
#include "oos/input/key_input.h"
#include "oos/runtime/native_app_manager.h"

namespace oos::platform {
namespace {

using oos::compositor::Compositor;
using oos::device::Device;
using oos::device::Display;
using oos::input::KeyEvent;
using oos::input::KeyInput;
using oos::runtime::NativeAppManager;

constexpr const char *kDefaultBootSplash = "/opt/oos/share/oos/boot-splash.png";
constexpr const char *kDefaultLauncherModule = "/opt/oos/apps/launcher.aot";
constexpr int kFrameIntervalMs = 33;

volatile std::sig_atomic_t g_stop_requested = 0;

const char *environmentOr(const char *name, const char *fallback) {
  const char *value = std::getenv(name);
  return value && value[0] != '\0' ? value : fallback;
}

int64_t monotonicMicros() {
  timespec time = {};
  clock_gettime(CLOCK_MONOTONIC, &time);
  return static_cast<int64_t>(time.tv_sec) * 1000000 + time.tv_nsec / 1000;
}

void stopRuntime(int) { g_stop_requested = 1; }

bool loadBootSplash(const char *path, uint32_t expected_width,
                    uint32_t expected_height, std::vector<uint16_t> &rgb565) {
  png_image image = {};
  image.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&image, path)) {
    std::fprintf(stderr, "failed to open boot splash %s: %s\n", path,
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
    std::fprintf(stderr, "failed to decode boot splash %s: %s\n", path,
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

struct InputContext {
  NativeAppManager *apps;
  bool success = true;
};

void dispatchKey(void *data, const KeyEvent &event) {
  auto *context = static_cast<InputContext *>(data);
  if (!context->apps->dispatchKey(event, monotonicMicros())) {
    std::fprintf(stderr, "WASM key dispatch failed: %s\n",
                 context->apps->lastError());
    context->success = false;
  }
}

} // namespace

int run(int argc, char **argv) {
  if (argc > 2) {
    std::fprintf(stderr, "usage: %s [APP.wasm|APP.aot]\n", argv[0]);
    return 2;
  }
  const char *module_path =
      argc == 2 ? argv[1]
                : environmentOr("OOS_LAUNCHER_MODULE", kDefaultLauncherModule);

  std::unique_ptr<Device> platform_device = device::createDevice();
  if (!platform_device || !platform_device->initialize()) {
    std::fprintf(stderr, "failed to initialize OOS device%s%s\n",
                 platform_device ? ": " : "",
                 platform_device ? platform_device->lastError().c_str()
                                 : "factory unavailable");
    return 1;
  }
  Display &display = platform_device->display();
  KeyInput &input = platform_device->keyInput();
  const device::DeviceDescriptor &descriptor = platform_device->descriptor();

  std::vector<uint16_t> boot_frame;
  const char *boot_path = environmentOr("OOS_BOOT_SPLASH", kDefaultBootSplash);
  if (!loadBootSplash(boot_path, descriptor.primary_width,
                      descriptor.primary_height, boot_frame) ||
      !display.showBootFrame(boot_frame.data())) {
    std::fprintf(stderr, "failed to present OOS boot splash\n");
    platform_device->shutdown();
    return 1;
  }

  Compositor compositor(display);
  NativeAppManager apps(compositor);
  if (!apps.load("launcher", module_path) || !apps.activate("launcher")) {
    std::fprintf(stderr, "failed to start native app %s: %s\n", module_path,
                 apps.lastError());
    platform_device->shutdown();
    return 1;
  }
  std::fprintf(stderr, "OOS native app started on %s: %s\n", descriptor.id,
               module_path);
  std::fflush(stderr);

  std::signal(SIGINT, stopRuntime);
  std::signal(SIGTERM, stopRuntime);
  InputContext input_context{&apps};
  auto next_frame = std::chrono::steady_clock::now();
  while (!g_stop_requested) {
    if (input.poll(0, dispatchKey, &input_context) < 0 ||
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

  apps.shutdown();
  platform_device->shutdown();
  return g_stop_requested ? 0 : 1;
}

} // namespace oos::platform
