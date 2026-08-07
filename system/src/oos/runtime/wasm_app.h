#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "oos/apps/app_manifest.h"
#include "oos/input/key_input.h"
#include "oos/runtime/application_context.h"

namespace oos::device {
class Device;
}

namespace oos::apps {
class AppRepository;
}

namespace oos::ui {
class StatusBarAppearanceController;
}

namespace oos::runtime {

class GraphicsHost;

struct WasmAppOptions : ApplicationContextOptions {
  uint32_t stack_size = 512 * 1024;
  uint32_t heap_size = 0;
  std::string module_directory;
  std::vector<apps::AppModule> modules;
};

class WasmApp {
public:
  explicit WasmApp(GraphicsHost &graphics, WasmAppOptions options = {});
  WasmApp(GraphicsHost &graphics, device::Device &device,
          WasmAppOptions options = {});
  ~WasmApp();

  WasmApp(const WasmApp &) = delete;
  WasmApp &operator=(const WasmApp &) = delete;

  bool load(const char *base_path);
  bool initialize();
  bool dispatchKey(const input::KeyEvent &event, int64_t monotonic_us);
  bool render(int64_t monotonic_us, uint32_t &next_delay_ms);
  bool render(int64_t monotonic_us) {
    uint32_t ignored_delay = 0;
    return render(monotonic_us, ignored_delay);
  }
  bool takeExitRequest();
  std::string takeLaunchRequest();
  std::string takeUninstallRequest();
  void setAudioFocused(bool focused);
  void shutdown();

  const char *lastError() const;
  bool loaded() const;
  const std::string &loadedArtifactPath() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::runtime
