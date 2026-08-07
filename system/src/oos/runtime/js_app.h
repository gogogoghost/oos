#pragma once

#include "oos/apps/app_manifest.h"
#include "oos/input/key_input.h"
#include "oos/runtime/application_context.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oos::device {
class Device;
}

namespace oos::runtime {

class GraphicsHost;

struct JsAppOptions : ApplicationContextOptions {
  std::string application_directory;
  std::string module_directory;
  std::vector<apps::AppModule> modules;
  size_t memory_limit = 16 * 1024 * 1024;
  size_t stack_limit = 512 * 1024;
  uint32_t execution_time_limit_ms = 50;
};

class JsApp {
public:
  explicit JsApp(GraphicsHost &graphics, JsAppOptions options = {});
  JsApp(GraphicsHost &graphics, device::Device &device,
        JsAppOptions options = {});
  ~JsApp();

  JsApp(const JsApp &) = delete;
  JsApp &operator=(const JsApp &) = delete;

  bool load(const char *path);
  bool initialize();
  bool dispatchKey(const input::KeyEvent &event, int64_t monotonic_us);
  bool render(int64_t monotonic_us, uint32_t &next_delay_ms);
  bool takeExitRequest();
  std::string takeLaunchRequest();
  std::string takeUninstallRequest();
  void setAudioFocused(bool focused);
  void shutdown();

  const char *lastError() const;
  bool loaded() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::runtime
