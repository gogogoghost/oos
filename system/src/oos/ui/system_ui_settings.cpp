#include "oos/ui/system_ui_settings.h"

#include "oos/apps/json.h"
#include "oos/storage/filesystem.h"

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace oos::ui {
namespace {

bool readBoolean(const apps::JsonValue &root, const char *name, bool &value,
                 std::string &error) {
  const apps::JsonValue *field = root.get(name);
  if (!field || !field->isBoolean()) {
    error = std::string("status bar setting '") + name + "' must be boolean";
    return false;
  }
  value = field->booleanValue();
  return true;
}

} // namespace

SystemUiSettings::SystemUiSettings(std::string data_root)
    : path_(std::move(data_root) + "/system/status-bar.json") {}

bool SystemUiSettings::initialize() {
  error_.clear();
  struct stat status = {};
  if (stat(path_.c_str(), &status) != 0) {
    if (errno == ENOENT) {
      status_bar_.revision = 1;
      return true;
    }
    error_ = "stat status bar settings failed";
    return false;
  }
  std::vector<uint8_t> bytes;
  if (!storage::readFile(path_, bytes, 4096, error_))
    return false;
  apps::JsonValue root;
  const std::string json(bytes.begin(), bytes.end());
  StatusBarPreferences parsed;
  if (!apps::parseJson(json, root, error_) || !root.isObject() ||
      !readBoolean(root, "clock", parsed.show_clock, error_) ||
      !readBoolean(root, "network", parsed.show_network, error_) ||
      !readBoolean(root, "batteryPercentage", parsed.show_battery_percentage,
                   error_)) {
    if (error_.empty())
      error_ = "status bar settings must contain an object";
    return false;
  }
  parsed.revision = status_bar_.revision + 1;
  status_bar_ = parsed;
  return true;
}

const StatusBarPreferences &SystemUiSettings::statusBar() const {
  return status_bar_;
}

bool SystemUiSettings::setStatusBar(bool show_clock, bool show_network,
                                    bool show_battery_percentage) {
  if (status_bar_.show_clock == show_clock &&
      status_bar_.show_network == show_network &&
      status_bar_.show_battery_percentage == show_battery_percentage)
    return true;
  StatusBarPreferences previous = status_bar_;
  status_bar_.show_clock = show_clock;
  status_bar_.show_network = show_network;
  status_bar_.show_battery_percentage = show_battery_percentage;
  ++status_bar_.revision;
  if (save())
    return true;
  status_bar_ = previous;
  return false;
}

bool SystemUiSettings::save() {
  char json[160] = {};
  const int count =
      std::snprintf(json, sizeof(json),
                    "{\n  \"clock\": %s,\n  \"network\": %s,\n  "
                    "\"batteryPercentage\": %s\n}\n",
                    status_bar_.show_clock ? "true" : "false",
                    status_bar_.show_network ? "true" : "false",
                    status_bar_.show_battery_percentage ? "true" : "false");
  if (count <= 0 || static_cast<size_t>(count) >= sizeof(json)) {
    error_ = "serialize status bar settings failed";
    return false;
  }
  return storage::writeFileAtomic(path_,
                                  reinterpret_cast<const uint8_t *>(json),
                                  static_cast<size_t>(count), 0600, error_);
}

const std::string &SystemUiSettings::lastError() const { return error_; }

} // namespace oos::ui
