#include "oos/ui/system_ui.h"

#include "oos/apps/app_repository.h"
#include "oos/runtime/graphics_host.h"
#include "oos/ui/icons.h"
#include "oos/ui/logo.h"
#include "oos/ui/lvgl_backend.h"
#include "oos/ui/system_status.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace oos::ui {
namespace {

constexpr uint16_t kKeyUp = 103;
constexpr uint16_t kKeyLeft = 105;
constexpr uint16_t kKeyRight = 106;
constexpr uint16_t kKeyDown = 108;
constexpr uint16_t kKeySoftLeft = 139;
constexpr uint16_t kKeyBack = 158;
constexpr uint16_t kKeyOk = 352;
constexpr uint16_t kKeySoftRight = 357;

lv_color_t color(uint32_t rgb) { return lv_color_hex(rgb); }

constexpr uint32_t kOrange = 0xe65100;
constexpr uint32_t kCanvas = 0x201f1d;
constexpr uint32_t kSurface = 0x292725;
constexpr uint32_t kSelectedSurface = 0x34302d;
constexpr uint32_t kStatusBackground = 0x171614;
constexpr uint32_t kText = 0xf0ede9;
constexpr uint32_t kMuted = 0xa9a29a;
constexpr uint32_t kDivider = 0x3d3935;
constexpr uint32_t kWhite = 0xffffff;
constexpr uint32_t kStatusInactive = 0x5f5a55;

struct AppVisual {
  const char *name;
  const char *symbol;
  uint32_t icon_color;
};

constexpr std::array<AppVisual, 6> kCoreApps = {
    {{"Phone", icons::kPhone, 0x2f7658},
     {"Messages", icons::kMessages, 0x35669a},
     {"Contacts", icons::kContacts, 0x6e527e},
     {"Camera", icons::kCamera, 0x9b493b},
     {"Files", icons::kFiles, 0x397174},
     {"Settings", icons::kSettings, 0x625e59}}};

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

std::string currentDate() {
  const std::time_t now = std::time(nullptr);
  std::tm local = {};
  localtime_r(&now, &local);
  char value[24] = {};
  std::strftime(value, sizeof(value), "%a, %b %d", &local);
  return value;
}

} // namespace

class SystemUi::Impl {
public:
  enum class View { Home, Apps };

  Impl(runtime::GraphicsHost &graphics, apps::AppRepository &repository,
       SystemStatusSource *status_source)
      : backend(graphics), repository(repository),
        status_source(status_source) {}

  bool initialize() {
    if (initialized)
      return true;
    if (!backend.initialize()) {
      error = backend.lastError();
      return false;
    }
    root = backend.root();
    if (!root) {
      error = "LVGL did not create a root screen";
      backend.shutdown();
      return false;
    }
    stripObject(root);
    lv_obj_set_style_bg_color(root, color(kCanvas), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    status = lv_obj_create(root);
    stripObject(status);
    lv_obj_set_size(status, LV_PCT(100), 22);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(status, color(kStatusBackground), 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);

    status_time = makeLabel(status, "00:00", &lv_font_montserrat_10, kText);
    lv_obj_align(status_time, LV_ALIGN_LEFT_MID, 6, 0);

    status_indicators = lv_obj_create(status);
    stripObject(status_indicators);
    lv_obj_set_size(status_indicators, 188, 22);
    lv_obj_align(status_indicators, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_opa(status_indicators, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_column(status_indicators, 5, 0);
    lv_obj_set_flex_flow(status_indicators, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_indicators, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    status_signal_container = lv_obj_create(status_indicators);
    stripObject(status_signal_container);
    lv_obj_set_size(status_signal_container, 13, 11);
    lv_obj_set_style_bg_opa(status_signal_container, LV_OPA_TRANSP, 0);
    for (size_t index = 0; index < status_signal.size(); ++index) {
      lv_obj_t *bar = lv_obj_create(status_signal_container);
      stripObject(bar);
      const int height = 3 + static_cast<int>(index) * 2;
      lv_obj_set_size(bar, 2, height);
      lv_obj_set_pos(bar, static_cast<int>(index) * 3, 11 - height);
      lv_obj_set_style_bg_color(bar, color(kStatusInactive), 0);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
      status_signal[index] = bar;
    }
    status_radio =
        makeLabel(status_indicators, "", &lv_font_montserrat_10, kText);
    status_wifi = makeLabel(status_indicators, icons::kWifi,
                            &lv_font_montserrat_10, kText);
    status_charge = makeLabel(status_indicators, icons::kCharge,
                              &lv_font_montserrat_10, kOrange);
    lv_obj_add_flag(status_radio, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_wifi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_charge, LV_OBJ_FLAG_HIDDEN);
    status_battery = makeLabel(status_indicators, icons::kBatteryEmpty,
                               &lv_font_montserrat_10, kStatusInactive);

    content = lv_obj_create(root);
    stripObject(content);
    lv_obj_set_size(content, LV_PCT(100), 272);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_bg_color(content, color(kCanvas), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);

    softkeys = lv_obj_create(root);
    stripObject(softkeys);
    lv_obj_set_size(softkeys, LV_PCT(100), 26);
    lv_obj_align(softkeys, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(softkeys, color(kSurface), 0);
    lv_obj_set_style_bg_opa(softkeys, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(softkeys, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(softkeys, 1, 0);
    lv_obj_set_style_border_color(softkeys, color(kDivider), 0);
    soft_left = makeLabel(softkeys, "", &lv_font_montserrat_10, kText);
    soft_center = makeLabel(softkeys, "", &lv_font_montserrat_10, kOrange);
    soft_right = makeLabel(softkeys, "", &lv_font_montserrat_10, kText);
    lv_obj_align(soft_left, LV_ALIGN_LEFT_MID, 7, 0);
    lv_obj_align(soft_center, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(soft_right, LV_ALIGN_RIGHT_MID, -7, 0);

    updateClock();
    updateStatus();
    showHome();
    needs_refresh = true;
    initialized = true;
    error.clear();
    return true;
  }

  void shutdown() {
    backend.shutdown();
    root = nullptr;
    status = nullptr;
    status_time = nullptr;
    status_indicators = nullptr;
    status_signal_container = nullptr;
    status_signal.fill(nullptr);
    status_radio = nullptr;
    status_wifi = nullptr;
    status_charge = nullptr;
    status_battery = nullptr;
    content = nullptr;
    softkeys = nullptr;
    soft_left = nullptr;
    soft_center = nullptr;
    soft_right = nullptr;
    home_time = nullptr;
    home_date = nullptr;
    app_cards.fill(nullptr);
    notice = nullptr;
    initialized = false;
    notice_until_us = 0;
    last_minute = -1;
    last_status_revision = 0;
    last_frame_us = 0;
    needs_refresh = false;
  }

  void showHome() {
    view = View::Home;
    selected = 0;
    notice = nullptr;
    app_cards.fill(nullptr);
    home_time = nullptr;
    home_date = nullptr;
    lv_obj_clean(content);

    lv_obj_t *mark = lv_image_create(content);
    lv_image_set_src(mark, &kLogoImage);
    lv_obj_set_pos(mark, 16, 20);

    lv_obj_t *name =
        makeLabel(content, "Orange OS", &lv_font_montserrat_14, kText);
    lv_obj_set_pos(name, 59, 19);
    lv_obj_t *system =
        makeLabel(content, "SYSTEM", &lv_font_montserrat_10, kOrange);
    lv_obj_set_pos(system, 59, 38);

    home_time = makeLabel(content, "00:00", &lv_font_montserrat_36, kText);
    lv_label_set_text(home_time, clock_text.c_str());
    lv_obj_set_pos(home_time, 13, 77);
    home_date =
        makeLabel(content, date_text.c_str(), &lv_font_montserrat_12, kMuted);
    lv_obj_set_pos(home_date, 16, 121);
    lv_label_set_text(soft_left, "Alerts");
    lv_label_set_text(soft_center, "Apps");
    lv_label_set_text(soft_right, "Camera");
    updateClock();
    applyStatus();
    needs_refresh = true;
  }

  void showApps() {
    view = View::Apps;
    notice = nullptr;
    app_cards.fill(nullptr);
    home_time = nullptr;
    home_date = nullptr;
    lv_obj_clean(content);

    for (size_t index = 0; index < kCoreApps.size(); ++index) {
      const AppVisual &app = kCoreApps[index];
      const int column = static_cast<int>(index % 3);
      const int row = static_cast<int>(index / 3);
      lv_obj_t *card = lv_obj_create(content);
      stripObject(card);
      lv_obj_set_size(card, 72, 88);
      lv_obj_set_pos(card, 7 + column * 77, 8 + row * 96);
      lv_obj_set_style_bg_color(card, color(kCanvas), 0);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(card, 2, 0);
      lv_obj_set_style_border_color(card, color(kOrange), 0);
      lv_obj_set_style_border_opa(card, LV_OPA_TRANSP, 0);
      lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);

      lv_obj_t *icon = lv_obj_create(card);
      stripObject(icon);
      lv_obj_set_size(icon, 38, 38);
      lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 10);
      lv_obj_set_style_bg_color(icon, color(app.icon_color), 0);
      lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
      lv_obj_t *symbol =
          makeLabel(icon, app.symbol, &lv_font_montserrat_20, kWhite);
      lv_obj_align(symbol, LV_ALIGN_CENTER, 0, 0);

      lv_obj_t *label =
          makeLabel(card, app.name, &lv_font_montserrat_10, kText);
      lv_obj_set_width(label, 68);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -10);
      app_cards[index] = card;
    }
    updateSelection();
    lv_label_set_text(soft_left, "Back");
    lv_label_set_text(soft_center, "Open");
    lv_label_set_text(soft_right, "Options");
    needs_refresh = true;
  }

  void updateSelection() {
    for (size_t index = 0; index < app_cards.size(); ++index) {
      lv_obj_t *card = app_cards[index];
      if (!card)
        continue;
      const bool active = index == selected;
      lv_obj_set_style_bg_color(card,
                                color(active ? kSelectedSurface : kCanvas), 0);
      lv_obj_set_style_border_opa(card, active ? LV_OPA_COVER : LV_OPA_TRANSP,
                                  0);
    }
    needs_refresh = true;
  }

  void updateClock() {
    const std::time_t now = std::time(nullptr);
    const int64_t minute = static_cast<int64_t>(now / 60);
    if (minute == last_minute)
      return;
    last_minute = minute;
    clock_text = currentTime();
    date_text = currentDate();
    if (status_time)
      lv_label_set_text(status_time, clock_text.c_str());
    if (home_time)
      lv_label_set_text(home_time, clock_text.c_str());
    if (home_date)
      lv_label_set_text(home_date, date_text.c_str());
    needs_refresh = true;
  }

  void applyStatus() {
    const bool cellular =
        system_status.cellular_available && system_status.cellular_registered;
    if (system_status.cellular_available)
      lv_obj_remove_flag(status_signal_container, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(status_signal_container, LV_OBJ_FLAG_HIDDEN);
    for (size_t index = 0; index < status_signal.size(); ++index) {
      const bool active =
          cellular && static_cast<int>(index) < system_status.signal_bars;
      lv_obj_set_style_bg_color(status_signal[index],
                                color(active ? kText : kStatusInactive), 0);
    }
    if (cellular && !system_status.radio_technology.empty()) {
      lv_label_set_text(status_radio, system_status.radio_technology.c_str());
      lv_obj_remove_flag(status_radio, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_label_set_text(status_radio, "");
      lv_obj_add_flag(status_radio, LV_OBJ_FLAG_HIDDEN);
    }
    if (system_status.wifi_connected)
      lv_obj_remove_flag(status_wifi, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(status_wifi, LV_OBJ_FLAG_HIDDEN);

    if (system_status.charging)
      lv_obj_remove_flag(status_charge, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(status_charge, LV_OBJ_FLAG_HIDDEN);

    const char *battery_symbol = icons::kBatteryEmpty;
    if (system_status.battery_percent >= 90)
      battery_symbol = icons::kBatteryFull;
    else if (system_status.battery_percent >= 65)
      battery_symbol = icons::kBatteryThreeQuarters;
    else if (system_status.battery_percent >= 35)
      battery_symbol = icons::kBatteryHalf;
    else if (system_status.battery_percent >= 10)
      battery_symbol = icons::kBatteryQuarter;
    char battery_text[24] = {};
    if (system_status.battery_available)
      std::snprintf(battery_text, sizeof(battery_text), "%d%% %s",
                    system_status.battery_percent, battery_symbol);
    else
      std::snprintf(battery_text, sizeof(battery_text), "%s", battery_symbol);
    lv_label_set_text(status_battery, battery_text);
    lv_obj_set_style_text_color(
        status_battery,
        color(system_status.battery_available ? kText : kStatusInactive), 0);

    needs_refresh = true;
  }

  void updateStatus() {
    if (!status_source)
      return;
    SystemStatusSnapshot next = status_source->snapshot();
    if (next.revision == last_status_revision)
      return;
    last_status_revision = next.revision;
    system_status = std::move(next);
    applyStatus();
  }

  void showNotice(const char *message, int64_t monotonic_us) {
    if (notice)
      lv_obj_delete(notice);
    notice = lv_obj_create(content);
    stripObject(notice);
    lv_obj_set_size(notice, 224, 30);
    lv_obj_align(notice, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(notice, color(kSurface), 0);
    lv_obj_set_style_bg_opa(notice, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(notice, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(notice, 3, 0);
    lv_obj_set_style_border_color(notice, color(kOrange), 0);
    lv_obj_t *label = makeLabel(notice, message, &lv_font_montserrat_10, kText);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_to_index(notice, -1);
    notice_until_us = monotonic_us + 1400000;
    needs_refresh = true;
  }

  bool dispatchKey(const input::KeyEvent &event) {
    if (!initialized)
      return false;
    backend.dispatchKey(event);
    if (event.action == input::KeyAction::Released)
      return true;
    const int64_t now =
        event.timestamp_us > 0 ? event.timestamp_us : last_frame_us;
    if (view == View::Home) {
      if (event.code == kKeyOk) {
        showApps();
      } else if (event.code == kKeySoftLeft) {
        showNotice("No alerts", now);
      } else if (event.code == kKeySoftRight) {
        showNotice("Camera is not installed", now);
      }
      return true;
    }

    switch (event.code) {
    case kKeyBack:
    case kKeySoftLeft:
      showHome();
      break;
    case kKeyLeft:
      selected = (selected + kCoreApps.size() - 1) % kCoreApps.size();
      updateSelection();
      break;
    case kKeyRight:
      selected = (selected + 1) % kCoreApps.size();
      updateSelection();
      break;
    case kKeyUp:
      selected = (selected + kCoreApps.size() - 3) % kCoreApps.size();
      updateSelection();
      break;
    case kKeyDown:
      selected = (selected + 3) % kCoreApps.size();
      updateSelection();
      break;
    case kKeyOk:
      showNotice("App is not installed", now);
      break;
    case kKeySoftRight:
      showNotice("Options are not available", now);
      break;
    default:
      break;
    }
    return true;
  }

  bool frame(int64_t monotonic_us, uint32_t &next_delay_ms) {
    if (!initialized)
      return false;
    last_frame_us = monotonic_us;
    updateClock();
    updateStatus();
    if (notice && monotonic_us >= notice_until_us) {
      lv_obj_delete(notice);
      notice = nullptr;
      notice_until_us = 0;
      needs_refresh = true;
    }
    backend.frame(monotonic_us);
    if (!backend.healthy()) {
      error = backend.lastError();
      return false;
    }
    if (needs_refresh) {
      if (!backend.refresh()) {
        error = backend.lastError();
        return false;
      }
      needs_refresh = false;
    }
    const std::time_t wall_time = std::time(nullptr);
    const uint32_t seconds = static_cast<uint32_t>(wall_time % 60);
    next_delay_ms = std::max(1u, (60u - seconds) * 1000u);
    if (status_source)
      next_delay_ms = std::min(next_delay_ms, 1000u);
    if (notice) {
      const int64_t remaining_us = notice_until_us - monotonic_us;
      if (remaining_us > 0) {
        const uint32_t remaining_ms =
            static_cast<uint32_t>((remaining_us + 999) / 1000);
        next_delay_ms = std::min(next_delay_ms, remaining_ms);
      }
    }
    return true;
  }

  LvglBackend backend;
  apps::AppRepository &repository;
  lv_obj_t *root = nullptr;
  lv_obj_t *status = nullptr;
  lv_obj_t *status_time = nullptr;
  lv_obj_t *status_indicators = nullptr;
  lv_obj_t *status_signal_container = nullptr;
  std::array<lv_obj_t *, 4> status_signal = {};
  lv_obj_t *status_radio = nullptr;
  lv_obj_t *status_wifi = nullptr;
  lv_obj_t *status_charge = nullptr;
  lv_obj_t *status_battery = nullptr;
  lv_obj_t *content = nullptr;
  lv_obj_t *softkeys = nullptr;
  lv_obj_t *soft_left = nullptr;
  lv_obj_t *soft_center = nullptr;
  lv_obj_t *soft_right = nullptr;
  lv_obj_t *home_time = nullptr;
  lv_obj_t *home_date = nullptr;
  lv_obj_t *notice = nullptr;
  std::array<lv_obj_t *, 6> app_cards = {};
  std::string clock_text;
  std::string date_text;
  std::string error;
  SystemStatusSnapshot system_status;
  SystemStatusSource *status_source = nullptr;
  int64_t notice_until_us = 0;
  int64_t last_frame_us = 0;
  int64_t last_minute = -1;
  uint64_t last_status_revision = 0;
  size_t selected = 0;
  View view = View::Home;
  bool initialized = false;
  bool needs_refresh = false;
};

SystemUi::SystemUi(runtime::GraphicsHost &graphics,
                   apps::AppRepository &repository,
                   SystemStatusSource *status_source)
    : impl_(std::make_unique<Impl>(graphics, repository, status_source)) {}

SystemUi::~SystemUi() { shutdown(); }

bool SystemUi::initialize() { return impl_->initialize(); }

void SystemUi::shutdown() {
  if (impl_)
    impl_->shutdown();
}

bool SystemUi::dispatchKey(const input::KeyEvent &event) {
  return impl_->dispatchKey(event);
}

bool SystemUi::frame(int64_t monotonic_us, uint32_t &next_delay_ms) {
  return impl_->frame(monotonic_us, next_delay_ms);
}

const std::string &SystemUi::lastError() const { return impl_->error; }

} // namespace oos::ui
