#pragma once

#include <cstdint>

namespace oos::ui {

enum class SurfaceMode : uint8_t { Normal = 0, Immersive = 1 };

struct StatusBarAppearance {
  uint32_t background_rgb = 0;
  bool dark_icons = false;

  bool operator==(const StatusBarAppearance &other) const {
    return background_rgb == other.background_rgb &&
           dark_icons == other.dark_icons;
  }
  bool operator!=(const StatusBarAppearance &other) const {
    return !(*this == other);
  }
};

class StatusBarAppearanceHost {
public:
  virtual ~StatusBarAppearanceHost() = default;
  virtual void applyStatusBarAppearance(StatusBarAppearance appearance) = 0;
  virtual void setStatusBarVisible(bool visible) { (void)visible; }
};

class StatusBarAppearanceController {
public:
  virtual ~StatusBarAppearanceController() = default;
  virtual void setStatusBarAppearance(StatusBarAppearance appearance) = 0;
  virtual StatusBarAppearance statusBarAppearance() const = 0;
  virtual bool setSurfaceMode(SurfaceMode) { return false; }
  virtual SurfaceMode surfaceMode() const { return SurfaceMode::Normal; }
};

} // namespace oos::ui
