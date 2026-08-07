#include "oos/ui/system_ui_state.h"

#include "oos/ui/system_ui_settings.h"

#include <cstdio>

namespace oos::ui {

namespace {

void appendJsonString(std::string &output, const std::string &value) {
  constexpr char hex[] = "0123456789abcdef";
  output.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
    case '"': output += "\\\""; break;
    case '\\': output += "\\\\"; break;
    case '\b': output += "\\b"; break;
    case '\f': output += "\\f"; break;
    case '\n': output += "\\n"; break;
    case '\r': output += "\\r"; break;
    case '\t': output += "\\t"; break;
    default:
      if (character < 0x20) {
        output += "\\u00";
        output.push_back(hex[character >> 4]);
        output.push_back(hex[character & 0x0f]);
      } else {
        output.push_back(static_cast<char>(character));
      }
    }
  }
  output.push_back('"');
}

const char *boolean(bool value) { return value ? "true" : "false"; }

} // namespace

SystemUiState::SystemUiState(SystemStatusSource *status_source,
                             SystemUiSettings *settings)
    : status_source_(status_source), settings_(settings) {}

void SystemUiState::applyStatusBarAppearance(StatusBarAppearance appearance) {
  appearance.background_rgb &= 0x00ffffffu;
  if (appearance_ != appearance) {
    appearance_ = appearance;
    ++revision_;
  }
}

void SystemUiState::setStatusBarVisible(bool visible) {
  if (status_bar_visible_ != visible) {
    status_bar_visible_ = visible;
    ++revision_;
  }
}

void SystemUiState::setStatusBarAppearance(StatusBarAppearance appearance) {
  applyStatusBarAppearance(appearance);
}

StatusBarAppearance SystemUiState::statusBarAppearance() const {
  return appearance_;
}

bool SystemUiState::setSurfaceMode(SurfaceMode mode) {
  if (surface_mode_ != mode) {
    surface_mode_ = mode;
    setStatusBarVisible(mode != SurfaceMode::Immersive);
  }
  return true;
}

SurfaceMode SystemUiState::surfaceMode() const { return surface_mode_; }

bool SystemUiState::snapshotJson(std::string &json) const {
  const SystemUiSnapshot value = snapshot();
  json = "{\"revision\":" + std::to_string(value.revision) +
         ",\"statusBarVisible\":" + boolean(value.status_bar_visible) +
         ",\"locked\":" + boolean(value.locked) +
         ",\"showClock\":" + boolean(value.preferences.show_clock) +
         ",\"showNetwork\":" + boolean(value.preferences.show_network) +
         ",\"showBatteryPercentage\":" +
         boolean(value.preferences.show_battery_percentage) +
         ",\"backgroundRgb\":" +
         std::to_string(value.appearance.background_rgb) +
         ",\"darkIcons\":" + boolean(value.appearance.dark_icons) +
         ",\"batteryAvailable\":" +
         boolean(value.status.battery_available) +
         ",\"batteryPercent\":" +
         std::to_string(value.status.battery_percent) +
         ",\"charging\":" + boolean(value.status.charging) +
         ",\"wifiAvailable\":" + boolean(value.status.wifi_available) +
         ",\"wifiConnected\":" + boolean(value.status.wifi_connected) +
         ",\"cellularAvailable\":" +
         boolean(value.status.cellular_available) +
         ",\"cellularRegistered\":" +
         boolean(value.status.cellular_registered) +
         ",\"roaming\":" + boolean(value.status.roaming) +
         ",\"signalBars\":" + std::to_string(value.status.signal_bars) +
         ",\"radioTechnology\":";
  appendJsonString(json, value.status.radio_technology);
  json.push_back('}');
  return true;
}

SystemUiSnapshot SystemUiState::snapshot() const {
  SystemUiSnapshot value;
  value.status =
      status_source_ ? status_source_->snapshot() : SystemStatusSnapshot{};
  value.preferences =
      settings_ ? settings_->statusBar() : StatusBarPreferences{};
  value.revision = revision_ + value.status.revision +
                   value.preferences.revision;
  value.status_bar_visible = status_bar_visible_;
  value.locked = locked_;
  value.appearance = appearance_;
  return value;
}

void SystemUiState::setLocked(bool locked) {
  if (locked_ != locked) {
    locked_ = locked;
    ++revision_;
  }
}

bool SystemUiState::locked() const { return locked_; }

} // namespace oos::ui
