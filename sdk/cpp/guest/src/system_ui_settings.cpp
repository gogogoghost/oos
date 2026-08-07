#include "oos/ui/system_ui_settings.h"

#include "app.h"

namespace oos::ui {

SystemUiSettings::SystemUiSettings(std::string data_root)
    : path_(std::move(data_root)) {}

bool SystemUiSettings::initialize() {
  oos_platform_system_settings_status_bar_preferences_t value{};
  oos_platform_system_settings_error_code_t error{};
  if (!oos_platform_system_settings_get_status_bar(&value, &error)) {
    error_ = "read status bar settings failed";
    return false;
  }
  status_bar_ = {value.show_clock, value.show_network,
                 value.show_battery_percentage, value.revision};
  error_.clear();
  return true;
}

const StatusBarPreferences &SystemUiSettings::statusBar() const {
  return status_bar_;
}

bool SystemUiSettings::setStatusBar(bool show_clock, bool show_network,
                                    bool show_battery_percentage) {
  oos_platform_system_settings_error_code_t error{};
  if (!oos_platform_system_settings_set_status_bar(
          show_clock, show_network, show_battery_percentage, &error)) {
    error_ = "write status bar settings failed";
    return false;
  }
  status_bar_.show_clock = show_clock;
  status_bar_.show_network = show_network;
  status_bar_.show_battery_percentage = show_battery_percentage;
  ++status_bar_.revision;
  error_.clear();
  return true;
}

void SystemUiSettings::updateFromHost(StatusBarPreferences status_bar) {
  status_bar_ = status_bar;
  error_.clear();
}

const std::string &SystemUiSettings::lastError() const { return error_; }
bool SystemUiSettings::save() { return true; }

} // namespace oos::ui
