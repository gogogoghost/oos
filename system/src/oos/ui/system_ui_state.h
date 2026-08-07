#pragma once

#include "oos/ui/status_bar_appearance.h"
#include "oos/ui/system_status.h"
#include "oos/ui/system_ui_settings.h"

#include <cstdint>
#include <string>

namespace oos::ui {

struct SystemUiSnapshot {
  uint64_t revision = 0;
  bool status_bar_visible = true;
  bool locked = false;
  StatusBarAppearance appearance{};
  StatusBarPreferences preferences{};
  SystemStatusSnapshot status{};
};

// Host mechanism shared with the packaged SystemUI application. It contains
// no rendering policy; the application reads one bulk snapshot and draws it.
class SystemUiState final : public StatusBarAppearanceHost,
                            public StatusBarAppearanceController {
public:
  explicit SystemUiState(SystemStatusSource *status_source = nullptr,
                         SystemUiSettings *settings = nullptr);

  void applyStatusBarAppearance(StatusBarAppearance appearance) override;
  void setStatusBarVisible(bool visible) override;
  void setStatusBarAppearance(StatusBarAppearance appearance) override;
  StatusBarAppearance statusBarAppearance() const override;
  bool setSurfaceMode(SurfaceMode mode) override;
  SurfaceMode surfaceMode() const override;

  bool snapshotJson(std::string &json) const;
  SystemUiSnapshot snapshot() const;
  void setLocked(bool locked);
  bool locked() const;

private:
  SystemStatusSource *status_source_ = nullptr;
  SystemUiSettings *settings_ = nullptr;
  StatusBarAppearance appearance_{0x101214, false};
  SurfaceMode surface_mode_ = SurfaceMode::Normal;
  uint64_t revision_ = 1;
  bool status_bar_visible_ = true;
  bool locked_ = false;
};

} // namespace oos::ui
