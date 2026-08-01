#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "oos/input/key_input.h"

namespace oos::device {
class Device;
}

namespace oos::apps {
class AppRepository;
}

namespace oos::runtime {

class GraphicsHost;

struct WasmAppOptions {
  uint32_t stack_size = 128 * 1024;
  uint32_t heap_size = 0;
  std::string app_id;
  std::string data_directory;
  std::string system_data_root;
  apps::AppRepository *app_repository = nullptr;
  std::string internal_media_directory = "/data/media/internal";
  std::string removable_media_directory = "/data/media/removable";
  std::string font_directory = "/opt/oos/share/fonts";
  uint32_t service_permission_mask = 0;
  bool enforce_service_permissions = false;
};

class WasmApp {
public:
  explicit WasmApp(GraphicsHost &graphics, WasmAppOptions options = {});
  WasmApp(GraphicsHost &graphics, device::Device &device,
          WasmAppOptions options = {});
  ~WasmApp();

  WasmApp(const WasmApp &) = delete;
  WasmApp &operator=(const WasmApp &) = delete;

  bool load(const char *path);
  bool initialize();
  bool dispatchKey(const input::KeyEvent &event, int64_t monotonic_us);
  bool render(int64_t monotonic_us);
  void shutdown();

  const char *lastError() const;
  bool loaded() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::runtime
