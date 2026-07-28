#pragma once

#include <cstdint>
#include <memory>

#include "oos/input/key_input.h"

namespace oos::device {
class Device;
}

namespace oos::runtime {

class GraphicsHost;

struct WasmAppOptions {
  uint32_t stack_size = 128 * 1024;
  uint32_t heap_size = 4 * 1024 * 1024;
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
