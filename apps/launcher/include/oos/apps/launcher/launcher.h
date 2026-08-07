#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "oos/window/input_router.h"

namespace oos::apps {
class AppRepository;
}

namespace oos::runtime {
class GraphicsHost;
}

namespace oos::ui {
class StatusBarAppearanceController;
}

namespace oos::apps::launcher {

// LVGL home application. It owns desktop content and softkeys only; status and
// global overlays belong to the independent SystemUI application package.
class Launcher final : public window::ApplicationInputTarget {
public:
  Launcher(runtime::GraphicsHost &graphics, AppRepository &repository,
           ui::StatusBarAppearanceController &status_bar);
  ~Launcher();

  Launcher(const Launcher &) = delete;
  Launcher &operator=(const Launcher &) = delete;

  bool initialize();
  void shutdown();
  bool dispatchKey(const input::KeyEvent &event, int64_t monotonic_us) override;
  bool frame(int64_t monotonic_us, uint32_t &next_delay_ms);
  std::string takeLaunchRequest();
  const char *lastError() const override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::apps::launcher
