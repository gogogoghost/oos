#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "oos/input/key_input.h"

namespace oos::apps {
class AppRepository;
}

namespace oos::runtime {
class GraphicsHost;
}

namespace oos::ui {

class SystemStatusSource;

// Trusted, process-local shell shown when OOS starts without an application
// argument. It owns the status bar, home screen and application grid.
class SystemUi {
public:
  SystemUi(runtime::GraphicsHost &graphics, apps::AppRepository &repository,
           SystemStatusSource *status_source = nullptr);
  ~SystemUi();

  SystemUi(const SystemUi &) = delete;
  SystemUi &operator=(const SystemUi &) = delete;

  bool initialize();
  void shutdown();
  bool dispatchKey(const input::KeyEvent &event);
  bool frame(int64_t monotonic_us, uint32_t &next_delay_ms);

  const std::string &lastError() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::ui
