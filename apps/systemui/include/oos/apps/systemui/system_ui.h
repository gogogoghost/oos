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

namespace oos::ui {
class SystemStatusSource;
class SystemUiSettings;
} // namespace oos::ui

namespace oos::apps::systemui {

// Trusted, process-local system shell. It owns two compositor layers: a
// non-focusable status bar and a modal/transient overlay. Launcher content is
// intentionally owned by a separate application.
class SystemUi : public window::SystemInputTarget,
                 public ui::StatusBarAppearanceHost {
public:
  SystemUi(compositor::LayerSurface &status_surface,
           compositor::LayerSurface &overlay_surface,
           ui::SystemStatusSource *status_source = nullptr,
           ui::SystemUiSettings *settings = nullptr);
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

  const std::string &lastError() const override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::apps::systemui
