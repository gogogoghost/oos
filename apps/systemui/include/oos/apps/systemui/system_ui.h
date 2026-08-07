#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "oos/input/key_input.h"
#include "oos/ui/status_bar_appearance.h"
#include "oos/window/input_router.h"

namespace oos::compositor {
class LayerSurface;
}

namespace oos::runtime {
class GraphicsHost;
}

namespace oos::ui {
class SystemStatusSource;
class SystemUiSettings;
} // namespace oos::ui

namespace oos::apps::systemui {

// LVGL system shell. Native builds use two compositor layers; the packaged
// guest maps the same status and overlay trees onto one transparent surface.
class SystemUi : public window::SystemInputTarget,
                 public ui::StatusBarAppearanceHost {
public:
#ifdef OOS_WASM_GUEST
  SystemUi(runtime::GraphicsHost &surface,
           ui::SystemStatusSource *status_source = nullptr,
           ui::SystemUiSettings *settings = nullptr);
#else
  SystemUi(compositor::LayerSurface &status_surface,
           compositor::LayerSurface &overlay_surface,
           ui::SystemStatusSource *status_source = nullptr,
           ui::SystemUiSettings *settings = nullptr);
#endif
  ~SystemUi();

  SystemUi(const SystemUi &) = delete;
  SystemUi &operator=(const SystemUi &) = delete;

  bool initialize();
  void shutdown();
  bool routeKey(const input::KeyEvent &event, bool &consumed) override;
  bool frame(int64_t monotonic_us, uint32_t &next_delay_ms);
  void showNotification(const std::string &message, int64_t monotonic_us,
                        uint32_t duration_ms = 3000);
  void setLocked(bool locked);
  bool locked() const;
  void applyStatusBarAppearance(ui::StatusBarAppearance appearance) override;
  void setStatusBarVisible(bool visible) override;

  const std::string &lastError() const override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::apps::systemui
