#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <lvgl.h>

#include "oos/input/key_input.h"

namespace oos::runtime {
class GraphicsHost;
}

namespace oos::sdk::ui {

struct LvglBackendOptions {
  bool transparent = false;
};

class LvglBackend {
public:
  explicit LvglBackend(runtime::GraphicsHost &graphics,
                       LvglBackendOptions options = {});
  ~LvglBackend();

  LvglBackend(const LvglBackend &) = delete;
  LvglBackend &operator=(const LvglBackend &) = delete;

  bool initialize();
  void shutdown();
  bool dispatchKey(const input::KeyEvent &event);
  uint32_t frame(int64_t monotonic_us);
  bool refresh();
  bool healthy() const;

  lv_obj_t *root() const;
  lv_group_t *group() const;
  const std::string &lastError() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::sdk::ui
