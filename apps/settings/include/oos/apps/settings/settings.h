#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "oos/window/input_router.h"

namespace oos::apps {
class AppRepository;
}

namespace oos::device {
class Device;
}

namespace oos::runtime {
class GraphicsHost;
}

namespace oos::ui {
class SystemUiSettings;
class StatusBarAppearanceController;
} // namespace oos::ui

namespace oos::apps::settings {

class Settings final : public window::ApplicationInputTarget {
public:
  Settings(runtime::GraphicsHost &graphics, AppRepository &repository,
           const device::Device &device, ui::SystemUiSettings &system,
           ui::StatusBarAppearanceController &status_bar,
           std::string data_root);
  ~Settings();

  Settings(const Settings &) = delete;
  Settings &operator=(const Settings &) = delete;

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

} // namespace oos::apps::settings
