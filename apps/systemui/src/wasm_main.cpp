#include "app.h"
#include "oos/apps/systemui/system_ui.h"
#include "oos/sdk/guest/platform.h"
#include "oos/ui/system_status.h"
#include "oos/ui/system_ui_settings.h"

#include <memory>

namespace {

class SnapshotSource final : public oos::ui::SystemStatusSource {
public:
  bool refresh() {
    oos_platform_system_ui_status_snapshot_t value{};
    oos_platform_system_ui_error_code_t error{};
    if (!oos_platform_system_ui_snapshot(&value, &error))
      return false;
    snapshot_.revision = value.revision;
    snapshot_.battery_available = value.battery_available;
    snapshot_.battery_percent = value.battery_percent;
    snapshot_.charging = value.charging;
    snapshot_.wifi_available = value.wifi_available;
    snapshot_.wifi_connected = value.wifi_connected;
    snapshot_.cellular_available = value.cellular_available;
    snapshot_.cellular_registered = value.cellular_registered;
    snapshot_.roaming = value.roaming;
    snapshot_.signal_bars = value.signal_bars;
    snapshot_.radio_technology =
        value.radio_technology.ptr
            ? std::string(
                  reinterpret_cast<const char *>(value.radio_technology.ptr),
                  value.radio_technology.len)
            : std::string();
    preferences_ = {value.show_clock, value.show_network,
                    value.show_battery_percentage, value.revision};
    status_bar_visible_ = value.status_bar_visible;
    locked_ = value.locked;
    appearance_ = {value.background_rgb, value.dark_icons};
    oos_platform_system_ui_status_snapshot_free(&value);
    return true;
  }

  oos::ui::SystemStatusSnapshot snapshot() const override { return snapshot_; }
  bool statusBarVisible() const { return status_bar_visible_; }
  bool locked() const { return locked_; }
  oos::ui::StatusBarAppearance appearance() const { return appearance_; }
  oos::ui::StatusBarPreferences preferences() const { return preferences_; }

private:
  oos::ui::SystemStatusSnapshot snapshot_{};
  oos::ui::StatusBarAppearance appearance_{};
  oos::ui::StatusBarPreferences preferences_{};
  bool status_bar_visible_ = true;
  bool locked_ = false;
};

std::unique_ptr<oos::sdk::guest::GraphicsHost> graphics;
std::unique_ptr<SnapshotSource> status_source;
std::unique_ptr<oos::ui::SystemUiSettings> system_settings;
std::unique_ptr<oos::apps::systemui::SystemUi> system_ui;

oos::input::KeyEvent
convert(const exports_oos_platform_lifecycle_key_event_t &event) {
  return {static_cast<int64_t>(oos_platform_runtime_monotonic_time_us()),
          static_cast<uint16_t>(event.code),
          static_cast<oos::input::KeyAction>(event.action),
          {},
          {}};
}

bool publishLockState() {
  oos_platform_system_ui_error_code_t error{};
  return oos_platform_system_ui_set_locked(system_ui->locked(), &error);
}

void applySnapshot() {
  system_settings->updateFromHost(status_source->preferences());
  system_ui->applyStatusBarAppearance(status_source->appearance());
  system_ui->setStatusBarVisible(status_source->statusBarVisible());
  if (system_ui->locked() != status_source->locked())
    system_ui->setLocked(status_source->locked());
}

} // namespace

bool exports_oos_platform_lifecycle_init(
    exports_oos_platform_lifecycle_error_code_t *error) {
  graphics = std::make_unique<oos::sdk::guest::GraphicsHost>();
  status_source = std::make_unique<SnapshotSource>();
  system_settings = std::make_unique<oos::ui::SystemUiSettings>();
  if (!status_source->refresh()) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  system_ui = std::make_unique<oos::apps::systemui::SystemUi>(
      *graphics, status_source.get(), system_settings.get());
  if (!system_ui->initialize()) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  applySnapshot();
  return true;
}

void exports_oos_platform_lifecycle_event(
    exports_oos_platform_lifecycle_key_event_t *event) {
  if (!system_ui || !event)
    return;
  bool consumed = false;
  const bool was_locked = system_ui->locked();
  system_ui->routeKey(convert(*event), consumed);
  if (was_locked != system_ui->locked())
    publishLockState();
}

bool exports_oos_platform_lifecycle_frame(
    uint64_t monotonic_time_us, uint32_t *next_delay_ms,
    exports_oos_platform_lifecycle_error_code_t *error) {
  if (!system_ui || !status_source->refresh()) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  applySnapshot();
  if (!system_ui->frame(static_cast<int64_t>(monotonic_time_us),
                        *next_delay_ms)) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  return true;
}

void exports_oos_platform_lifecycle_shutdown() {
  if (system_ui)
    system_ui->shutdown();
  system_ui.reset();
  system_settings.reset();
  status_source.reset();
  graphics.reset();
}
