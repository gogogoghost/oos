#include "oos/apps/systemui/system_ui.h"

#include "oos/compositor/compositor.h"
#include "oos/sdk/ui/fonts.h"
#include "oos/sdk/ui/icons.h"
#include "oos/sdk/ui/lvgl_backend.h"
#include "oos/sdk/ui/theme.h"
#include "oos/ui/system_status.h"
#include "oos/ui/system_ui_settings.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <utility>

namespace oos::apps::systemui {
namespace {

namespace sdk_ui = oos::sdk::ui;

constexpr uint16_t kKeyBack = 158;
constexpr uint16_t kKeyOk = 352;
constexpr uint32_t kOrange = sdk_ui::theme::kPrimaryLight;
constexpr uint32_t kStatusBackground = sdk_ui::theme::kStatusBackground;
constexpr uint32_t kText = sdk_ui::theme::kText;
constexpr uint32_t kStatusInactive = sdk_ui::theme::kStatusInactive;
constexpr uint32_t kDarkForeground = 0x17191b;
constexpr uint32_t kDarkInactive = 0x62686b;

lv_color_t color(uint32_t rgb) { return lv_color_hex(rgb); }

void stripObject(lv_obj_t *object) {
  lv_obj_set_style_border_width(object, 0, 0);
  lv_obj_set_style_radius(object, 0, 0);
  lv_obj_set_style_pad_all(object, 0, 0);
  lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, const lv_font_t *font,
                    uint32_t text_color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color(text_color), 0);
  return label;
}

std::string currentTime() {
  const std::time_t now = std::time(nullptr);
  std::tm local = {};
  localtime_r(&now, &local);
  char value[6] = {};
  std::strftime(value, sizeof(value), "%H:%M", &local);
  return value;
}

} // namespace

class SystemUi::Impl {
public:
  enum class OverlayMode { Hidden, Notification, Locked };

  Impl(compositor::LayerSurface &status_surface,
       compositor::LayerSurface &overlay_surface,
       ui::SystemStatusSource *status_source, ui::SystemUiSettings *settings)
      : status_backend(status_surface),
        overlay_backend(overlay_surface, sdk_ui::LvglBackendOptions{true}),
        status_surface(status_surface), overlay_surface(overlay_surface),
        status_source(status_source), settings(settings) {}

  bool initialize() {
    if (initialized)
      return true;
    if (!status_backend.initialize() || !overlay_backend.initialize()) {
      error = !status_backend.lastError().empty() ? status_backend.lastError()
                                                  : overlay_backend.lastError();
      shutdown();
      return false;
    }
    lv_obj_t *root = status_backend.root();
    if (!root) {
      error = "SystemUI status surface has no LVGL root";
      shutdown();
      return false;
    }
    stripObject(root);
    status_root = root;
    lv_obj_set_style_bg_color(root, color(appearance.background_rgb), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    status_time =
        makeLabel(root, currentTime().c_str(), sdk_ui::fonts::get(12), kText);
    lv_obj_align(status_time, LV_ALIGN_LEFT_MID, 7, 0);

    indicators = lv_obj_create(root);
    stripObject(indicators);
    lv_obj_set_size(indicators, 188, 22);
    lv_obj_align(indicators, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_opa(indicators, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_column(indicators, 5, 0);
    lv_obj_set_flex_flow(indicators, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicators, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    signal_container = lv_obj_create(indicators);
    stripObject(signal_container);
    lv_obj_set_size(signal_container, 13, 11);
    lv_obj_set_style_bg_opa(signal_container, LV_OPA_TRANSP, 0);
    for (size_t index = 0; index < signal.size(); ++index) {
      lv_obj_t *bar = lv_obj_create(signal_container);
      stripObject(bar);
      const int height = 3 + static_cast<int>(index) * 2;
      lv_obj_set_size(bar, 2, height);
      lv_obj_set_pos(bar, static_cast<int>(index) * 3, 11 - height);
      lv_obj_set_style_bg_color(bar, color(kStatusInactive), 0);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
      signal[index] = bar;
    }
    radio = makeLabel(indicators, "", sdk_ui::fonts::get(12), kText);
    wifi = makeLabel(indicators, sdk_ui::icons::kWifi, sdk_ui::fonts::get(12),
                     kText);
    charge = makeLabel(indicators, sdk_ui::icons::kCharge,
                       sdk_ui::fonts::get(12), kOrange);
    battery = makeLabel(indicators, sdk_ui::icons::kBatteryEmpty,
                        sdk_ui::fonts::get(12), kStatusInactive);
    lv_obj_add_flag(radio, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(charge, LV_OBJ_FLAG_HIDDEN);

    overlay_root = overlay_backend.root();
    if (!overlay_root) {
      error = "SystemUI overlay surface has no LVGL root";
      shutdown();
      return false;
    }
    stripObject(overlay_root);
    lv_obj_set_style_bg_opa(overlay_root, LV_OPA_TRANSP, 0);
    overlay_panel = lv_obj_create(overlay_root);
    stripObject(overlay_panel);
    overlay_title = makeLabel(overlay_panel, "Notification",
                              sdk_ui::fonts::get(12), kOrange);
    overlay_message =
        makeLabel(overlay_panel, "", sdk_ui::fonts::get(12), kText);
    overlay_time = makeLabel(overlay_panel, "", sdk_ui::fonts::get(36), kText);
    overlay_hint = makeLabel(overlay_panel, "Press OK to unlock",
                             sdk_ui::fonts::get(14), kOrange);
    lv_label_set_long_mode(overlay_message, LV_LABEL_LONG_DOT);
    lv_label_set_long_mode(overlay_hint, LV_LABEL_LONG_DOT);

    overlay_surface.setVisible(false);
    applyAppearance();
    updatePreferences();
    updateClock();
    updateStatus();
    status_needs_refresh = true;
    initialized = true;
    error.clear();
    return true;
  }

  void shutdown() {
    overlay_surface.setVisible(false);
    overlay_surface.clearFrame();
    overlay_backend.shutdown();
    status_backend.shutdown();
    status_root = nullptr;
    status_time = nullptr;
    indicators = nullptr;
    signal_container = nullptr;
    signal.fill(nullptr);
    radio = nullptr;
    wifi = nullptr;
    charge = nullptr;
    battery = nullptr;
    overlay_root = nullptr;
    overlay_panel = nullptr;
    overlay_title = nullptr;
    overlay_message = nullptr;
    overlay_time = nullptr;
    overlay_hint = nullptr;
    mode = OverlayMode::Hidden;
    initialized = false;
    status_needs_refresh = false;
  }

  void updateClock() {
    const int64_t minute = static_cast<int64_t>(std::time(nullptr) / 60);
    if (minute == last_minute)
      return;
    last_minute = minute;
    if (status_time)
      lv_label_set_text(status_time, currentTime().c_str());
    status_needs_refresh = true;
    if (mode == OverlayMode::Locked)
      overlay_needs_refresh = true;
  }

  void applyStatus() {
    const bool cellular =
        system_status.cellular_available && system_status.cellular_registered;
    if (preferences.show_network && system_status.cellular_available)
      lv_obj_remove_flag(signal_container, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(signal_container, LV_OBJ_FLAG_HIDDEN);
    for (size_t index = 0; index < signal.size(); ++index) {
      const bool active =
          cellular && static_cast<int>(index) < system_status.signal_bars;
      lv_obj_set_style_bg_color(
          signal[index], color(active ? foregroundColor() : inactiveColor()),
          0);
    }
    if (preferences.show_network && cellular &&
        !system_status.radio_technology.empty()) {
      lv_label_set_text(radio, system_status.radio_technology.c_str());
      lv_obj_remove_flag(radio, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(radio, LV_OBJ_FLAG_HIDDEN);
    }
    if (preferences.show_network && system_status.wifi_connected)
      lv_obj_remove_flag(wifi, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(wifi, LV_OBJ_FLAG_HIDDEN);
    if (system_status.battery_available && system_status.charging)
      lv_obj_remove_flag(charge, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(charge, LV_OBJ_FLAG_HIDDEN);

    const char *symbol = sdk_ui::icons::kBatteryEmpty;
    if (system_status.battery_percent >= 90)
      symbol = sdk_ui::icons::kBatteryFull;
    else if (system_status.battery_percent >= 65)
      symbol = sdk_ui::icons::kBatteryThreeQuarters;
    else if (system_status.battery_percent >= 35)
      symbol = sdk_ui::icons::kBatteryHalf;
    else if (system_status.battery_percent >= 10)
      symbol = sdk_ui::icons::kBatteryQuarter;
    char text[24] = {};
    if (system_status.battery_available && preferences.show_battery_percentage)
      std::snprintf(text, sizeof(text), "%d%% %s",
                    system_status.battery_percent, symbol);
    else
      std::snprintf(text, sizeof(text), "%s", symbol);
    lv_label_set_text(battery, text);
    lv_obj_set_style_text_color(battery,
                                color(system_status.battery_available
                                          ? foregroundColor()
                                          : inactiveColor()),
                                0);
    if (system_status.battery_available)
      lv_obj_remove_flag(battery, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(battery, LV_OBJ_FLAG_HIDDEN);
    status_needs_refresh = true;
  }

  uint32_t foregroundColor() const {
    return appearance.dark_icons ? kDarkForeground : kText;
  }

  uint32_t inactiveColor() const {
    return appearance.dark_icons ? kDarkInactive : kStatusInactive;
  }

  void applyAppearance() {
    if (!status_root)
      return;
    lv_obj_set_style_bg_color(status_root, color(appearance.background_rgb), 0);
    lv_obj_set_style_text_color(status_time, color(foregroundColor()), 0);
    lv_obj_set_style_text_color(radio, color(foregroundColor()), 0);
    lv_obj_set_style_text_color(wifi, color(foregroundColor()), 0);
    lv_obj_set_style_text_color(charge, color(foregroundColor()), 0);
    applyStatus();
    status_needs_refresh = true;
  }

  void setStatusBarAppearance(ui::StatusBarAppearance next) {
    next.background_rgb &= 0x00ffffffu;
    if (appearance == next)
      return;
    appearance = next;
    applyAppearance();
  }

  void updatePreferences() {
    if (!settings)
      return;
    const ui::StatusBarPreferences next = settings->statusBar();
    if (next.revision == last_preferences_revision)
      return;
    last_preferences_revision = next.revision;
    preferences = next;
    if (preferences.show_clock)
      lv_obj_remove_flag(status_time, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(status_time, LV_OBJ_FLAG_HIDDEN);
    applyStatus();
  }

  void updateStatus() {
    if (!status_source)
      return;
    ui::SystemStatusSnapshot next = status_source->snapshot();
    if (next.revision == last_status_revision)
      return;
    last_status_revision = next.revision;
    system_status = std::move(next);
    applyStatus();
  }

  bool renderOverlay(int64_t monotonic_us) {
    if (mode == OverlayMode::Hidden)
      return true;
    if (!overlay_root || !overlay_panel)
      return false;
    lv_obj_add_flag(overlay_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(overlay_message, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(overlay_time, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(overlay_hint, LV_OBJ_FLAG_HIDDEN);
    if (mode == OverlayMode::Locked) {
      const std::string time = currentTime();
      lv_obj_set_size(overlay_panel, overlay_surface.width(),
                      overlay_surface.height());
      lv_obj_align(overlay_panel, LV_ALIGN_CENTER, 0, 0);
      lv_obj_set_style_radius(overlay_panel, 0, 0);
      lv_obj_set_style_bg_color(overlay_panel, color(0x101214), 0);
      lv_obj_set_style_bg_opa(overlay_panel, LV_OPA_COVER, 0);
      lv_label_set_text(overlay_time, time.c_str());
      lv_obj_set_width(overlay_time, overlay_surface.width());
      lv_obj_set_style_text_align(overlay_time, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(overlay_time, LV_ALIGN_TOP_MID, 0, 64);
      lv_obj_set_width(overlay_hint, overlay_surface.width() - 20);
      lv_obj_set_style_text_align(overlay_hint, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(overlay_hint, LV_ALIGN_TOP_MID, 0, 136);
      lv_obj_remove_flag(overlay_time, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(overlay_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_set_size(overlay_panel, overlay_surface.width() - 16, 42);
      lv_obj_align(overlay_panel, LV_ALIGN_TOP_MID, 0, 8);
      lv_obj_set_style_radius(overlay_panel, 4, 0);
      lv_obj_set_style_bg_color(overlay_panel, color(0x202326), 0);
      lv_obj_set_style_bg_opa(overlay_panel, LV_OPA_COVER, 0);
      lv_obj_set_pos(overlay_title, 10, 12);
      lv_obj_set_width(overlay_title, 76);
      lv_label_set_text(overlay_message, notification.c_str());
      lv_obj_set_pos(overlay_message, 90, 12);
      lv_obj_set_width(overlay_message, overlay_surface.width() - 116);
      lv_obj_remove_flag(overlay_title, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(overlay_message, LV_OBJ_FLAG_HIDDEN);
    }
    overlay_backend.frame(monotonic_us);
    if (!overlay_backend.healthy() || !overlay_backend.refresh()) {
      error = overlay_backend.lastError();
      return false;
    }
    overlay_needs_refresh = false;
    return true;
  }

  bool frame(int64_t monotonic_us, uint32_t &next_delay_ms) {
    if (!initialized)
      return false;
    updateClock();
    updatePreferences();
    updateStatus();
    if (mode == OverlayMode::Notification &&
        monotonic_us >= notification_until_us) {
      mode = OverlayMode::Hidden;
      overlay_surface.setVisible(false);
      overlay_surface.clearFrame();
    }
    status_backend.frame(monotonic_us);
    if (!status_backend.healthy()) {
      error = status_backend.lastError();
      return false;
    }
    if (status_needs_refresh) {
      if (!status_backend.refresh()) {
        error = status_backend.lastError();
        return false;
      }
      status_needs_refresh = false;
    }
    if (mode != OverlayMode::Hidden && overlay_needs_refresh &&
        !renderOverlay(monotonic_us))
      return false;

    const uint32_t seconds = static_cast<uint32_t>(std::time(nullptr) % 60);
    next_delay_ms = std::max(1u, (60u - seconds) * 1000u);
    if (status_source)
      next_delay_ms = std::min(next_delay_ms, 1000u);
    if (mode == OverlayMode::Notification) {
      const int64_t remaining = notification_until_us - monotonic_us;
      if (remaining > 0)
        next_delay_ms = std::min(
            next_delay_ms, static_cast<uint32_t>((remaining + 999) / 1000));
    }
    return true;
  }

  bool routeKey(const input::KeyEvent &event, bool &consumed) {
    consumed = false;
    if (!initialized)
      return false;
    if (mode == OverlayMode::Locked) {
      consumed = true;
      if (event.action != input::KeyAction::Released && event.code == kKeyOk)
        setLocked(false);
      return true;
    }
    if (mode == OverlayMode::Notification &&
        event.action != input::KeyAction::Released && event.code == kKeyBack) {
      mode = OverlayMode::Hidden;
      overlay_surface.setVisible(false);
      overlay_surface.clearFrame();
      consumed = true;
    }
    return true;
  }

  void showNotification(const std::string &message, int64_t monotonic_us,
                        uint32_t duration_ms) {
    notification = message.empty() ? "New notification" : message;
    notification_until_us =
        monotonic_us + static_cast<int64_t>(duration_ms) * 1000;
    mode = OverlayMode::Notification;
    overlay_surface.setVisible(true);
    overlay_needs_refresh = true;
  }

  void setLocked(bool locked) {
    mode = locked ? OverlayMode::Locked : OverlayMode::Hidden;
    overlay_surface.setVisible(locked);
    if (!locked)
      overlay_surface.clearFrame();
    overlay_needs_refresh = locked;
  }

  sdk_ui::LvglBackend status_backend;
  sdk_ui::LvglBackend overlay_backend;
  compositor::LayerSurface &status_surface;
  compositor::LayerSurface &overlay_surface;
  ui::SystemStatusSource *status_source = nullptr;
  ui::SystemUiSettings *settings = nullptr;
  lv_obj_t *status_root = nullptr;
  lv_obj_t *status_time = nullptr;
  lv_obj_t *indicators = nullptr;
  lv_obj_t *signal_container = nullptr;
  std::array<lv_obj_t *, 4> signal = {};
  lv_obj_t *radio = nullptr;
  lv_obj_t *wifi = nullptr;
  lv_obj_t *charge = nullptr;
  lv_obj_t *battery = nullptr;
  lv_obj_t *overlay_root = nullptr;
  lv_obj_t *overlay_panel = nullptr;
  lv_obj_t *overlay_title = nullptr;
  lv_obj_t *overlay_message = nullptr;
  lv_obj_t *overlay_time = nullptr;
  lv_obj_t *overlay_hint = nullptr;
  ui::SystemStatusSnapshot system_status;
  ui::StatusBarPreferences preferences;
  ui::StatusBarAppearance appearance{kStatusBackground, false};
  std::string notification;
  std::string error;
  OverlayMode mode = OverlayMode::Hidden;
  int64_t notification_until_us = 0;
  int64_t last_minute = -1;
  uint64_t last_status_revision = UINT64_MAX;
  uint64_t last_preferences_revision = 0;
  bool initialized = false;
  bool status_needs_refresh = false;
  bool overlay_needs_refresh = false;
};

SystemUi::SystemUi(compositor::LayerSurface &status_surface,
                   compositor::LayerSurface &overlay_surface,
                   ui::SystemStatusSource *status_source,
                   ui::SystemUiSettings *settings)
    : impl_(std::make_unique<Impl>(status_surface, overlay_surface,
                                   status_source, settings)) {}

SystemUi::~SystemUi() { shutdown(); }

bool SystemUi::initialize() { return impl_->initialize(); }

void SystemUi::shutdown() {
  if (impl_)
    impl_->shutdown();
}

bool SystemUi::routeKey(const input::KeyEvent &event, bool &consumed) {
  return impl_->routeKey(event, consumed);
}

bool SystemUi::frame(int64_t monotonic_us, uint32_t &next_delay_ms) {
  return impl_->frame(monotonic_us, next_delay_ms);
}

void SystemUi::showNotification(const std::string &message,
                                int64_t monotonic_us, uint32_t duration_ms) {
  impl_->showNotification(message, monotonic_us, duration_ms);
}

void SystemUi::setLocked(bool locked) { impl_->setLocked(locked); }

bool SystemUi::locked() const {
  return impl_->mode == Impl::OverlayMode::Locked;
}

void SystemUi::applyStatusBarAppearance(ui::StatusBarAppearance appearance) {
  impl_->setStatusBarAppearance(appearance);
}

void SystemUi::setStatusBarVisible(bool visible) {
  impl_->status_surface.setVisible(visible);
}

const std::string &SystemUi::lastError() const { return impl_->error; }

} // namespace oos::apps::systemui
