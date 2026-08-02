#pragma once

#include <cstdint>
#include <string>

namespace oos::ui {

struct StatusBarPreferences {
  bool show_clock = true;
  bool show_network = true;
  bool show_battery_percentage = true;
  uint64_t revision = 0;
};

class SystemUiSettings {
public:
  explicit SystemUiSettings(std::string data_root = "/data");

  bool initialize();
  const StatusBarPreferences &statusBar() const;
  bool setStatusBar(bool show_clock, bool show_network,
                    bool show_battery_percentage);
  const std::string &lastError() const;

private:
  bool save();

  std::string path_;
  std::string error_;
  StatusBarPreferences status_bar_;
};

} // namespace oos::ui
