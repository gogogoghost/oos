#include "oos/ui/system_ui.h"

#include "oos/apps/app_repository.h"
#include "oos/runtime/graphics_host.h"
#include "oos/ui/lvgl_backend.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <string>
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

constexpr uint32_t kBackground = 0x11141a;
constexpr uint32_t kStatusBackground = 0x08090b;
constexpr uint32_t kPanel = 0x20242b;
constexpr uint32_t kPanelSelected = 0x2a2e35;
constexpr uint32_t kSoftkeyBackground = 0xf3f4f6;
constexpr uint32_t kOrange = 0xff7a00;
constexpr uint32_t kText = 0xf8f9fb;
constexpr uint32_t kMuted = 0xaeb3bc;

struct AppVisual {
  const char *name;
  const char *symbol;
  uint32_t color;
};

constexpr std::array<AppVisual, 6> kCoreApps = {{{"Phone", "P", 0x21a366},
                                                 {"Messages", "M", 0x2777d3},
                                                 {"Contacts", "C", 0x805ad5},
                                                 {"Camera", "C", 0xdc5a45},
                                                 {"Files", "F", 0x1597a5},
                                                 {"Settings", "S", 0x68727d}}};

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
  enum class View { Home, Apps };

  Impl(runtime::GraphicsHost &graphics, apps::AppRepository &repository)
      : backend(graphics), repository(repository) {}

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
    lv_obj_set_style_bg_color(root, color(kBackground), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    status = lv_obj_create(root);
    stripObject(status);
    lv_obj_set_size(status, LV_PCT(100), 20);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(status, color(kStatusBackground), 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);

    status_time = makeLabel(status, "00:00", &lv_font_montserrat_10, kText);
    lv_obj_align(status_time, LV_ALIGN_LEFT_MID, 7, 0);
    lv_obj_t *indicators =
        makeLabel(status, "WIFI  SIG  BAT", &lv_font_montserrat_10, 0xc7cbd2);
    lv_obj_align(indicators, LV_ALIGN_RIGHT_MID, -7, 0);

    content = lv_obj_create(root);
    stripObject(content);
    lv_obj_set_size(content, LV_PCT(100), 272);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_bg_color(content, color(kBackground), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);

    softkeys = lv_obj_create(root);
    stripObject(softkeys);
    lv_obj_set_size(softkeys, LV_PCT(100), 28);
    lv_obj_align(softkeys, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(softkeys, color(kSoftkeyBackground), 0);
    lv_obj_set_style_bg_opa(softkeys, LV_OPA_COVER, 0);
    soft_left = makeLabel(softkeys, "", &lv_font_montserrat_10, 0x15171a);
    soft_center = makeLabel(softkeys, "", &lv_font_montserrat_10, kOrange);
    soft_right = makeLabel(softkeys, "", &lv_font_montserrat_10, 0x15171a);
    lv_obj_align(soft_left, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_align(soft_center, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(soft_right, LV_ALIGN_RIGHT_MID, -6, 0);

    updateClock();
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
    content = nullptr;
    softkeys = nullptr;
    soft_left = nullptr;
    soft_center = nullptr;
    soft_right = nullptr;
    home_time = nullptr;
    app_cards.fill(nullptr);
    notice = nullptr;
    initialized = false;
    notice_until_us = 0;
    last_minute = -1;
    last_frame_us = 0;
    needs_refresh = false;
  }

  void showHome() {
    view = View::Home;
    selected = 0;
    notice = nullptr;
    app_cards.fill(nullptr);
    home_time = nullptr;
    lv_obj_clean(content);

    lv_obj_t *mark = lv_obj_create(content);
    stripObject(mark);
    lv_obj_set_size(mark, 47, 47);
    lv_obj_align(mark, LV_ALIGN_TOP_MID, 0, 39);
    lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mark, 3, 0);
    lv_obj_set_style_border_color(mark, color(kOrange), 0);
    lv_obj_t *mark_text = makeLabel(mark, "O", &lv_font_montserrat_20, kOrange);
    lv_obj_align(mark_text, LV_ALIGN_CENTER, 0, 0);

    home_time = makeLabel(content, "00:00", &lv_font_montserrat_36, kText);
    lv_label_set_text(home_time, clock_text.c_str());
    lv_obj_align(home_time, LV_ALIGN_TOP_MID, 0, 99);
    lv_obj_t *name =
        makeLabel(content, "Orange OS", &lv_font_montserrat_14, 0xc7cbd2);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 145);
    lv_obj_t *ready =
        makeLabel(content, "Orange OS   Ready", &lv_font_montserrat_10, kMuted);
    lv_obj_align(ready, LV_ALIGN_BOTTOM_MID, 0, -19);
    lv_label_set_text(soft_left, "Alerts");
    lv_label_set_text(soft_center, "Apps");
    lv_label_set_text(soft_right, "Camera");
    updateClock();
    needs_refresh = true;
  }

  void showApps() {
    view = View::Apps;
    notice = nullptr;
    app_cards.fill(nullptr);
    home_time = nullptr;
    lv_obj_clean(content);

    lv_obj_t *brand = makeLabel(content, "O", &lv_font_montserrat_20, kOrange);
    lv_obj_align(brand, LV_ALIGN_TOP_LEFT, 9, 7);
    lv_obj_t *title = makeLabel(content, "Apps", &lv_font_montserrat_20, kText);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 29, 7);

    for (size_t index = 0; index < kCoreApps.size(); ++index) {
      const AppVisual &app = kCoreApps[index];
      const int column = static_cast<int>(index % 3);
      const int row = static_cast<int>(index / 3);
      lv_obj_t *card = lv_obj_create(content);
      stripObject(card);
      lv_obj_set_size(card, 68, 81);
      lv_obj_set_pos(card, 9 + column * 74, 40 + row * 88);
      lv_obj_set_style_radius(card, 4, 0);
      lv_obj_set_style_bg_color(card, color(kPanel), 0);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
      lv_obj_set_style_border_color(card, color(kOrange), 0);
      lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);

      lv_obj_t *icon = lv_obj_create(card);
      stripObject(icon);
      lv_obj_set_size(icon, 36, 36);
      lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 8);
      lv_obj_set_style_radius(icon, 5, 0);
      lv_obj_set_style_bg_color(icon, color(app.color), 0);
      lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
      lv_obj_t *symbol =
          makeLabel(icon, app.symbol, &lv_font_montserrat_20, kText);
      lv_obj_align(symbol, LV_ALIGN_CENTER, 0, 0);

      lv_obj_t *label =
          makeLabel(card, app.name, &lv_font_montserrat_10, kText);
      lv_obj_set_width(label, 64);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -7);
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
      lv_obj_set_style_bg_color(card, color(active ? kPanelSelected : kPanel),
                                0);
      lv_obj_set_style_border_width(card, active ? 2 : 0, 0);
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
    if (status_time)
      lv_label_set_text(status_time, clock_text.c_str());
    if (home_time)
      lv_label_set_text(home_time, clock_text.c_str());
    needs_refresh = true;
  }

  void showNotice(const char *message, int64_t monotonic_us) {
    if (notice)
      lv_obj_delete(notice);
    notice = lv_obj_create(content);
    stripObject(notice);
    lv_obj_set_size(notice, 224, 30);
    lv_obj_align(notice, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(notice, color(0x090a0c), 0);
    lv_obj_set_style_bg_opa(notice, LV_OPA_90, 0);
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
  lv_obj_t *content = nullptr;
  lv_obj_t *softkeys = nullptr;
  lv_obj_t *soft_left = nullptr;
  lv_obj_t *soft_center = nullptr;
  lv_obj_t *soft_right = nullptr;
  lv_obj_t *home_time = nullptr;
  lv_obj_t *notice = nullptr;
  std::array<lv_obj_t *, 6> app_cards = {};
  std::string clock_text;
  std::string error;
  int64_t notice_until_us = 0;
  int64_t last_frame_us = 0;
  int64_t last_minute = -1;
  size_t selected = 0;
  View view = View::Home;
  bool initialized = false;
  bool needs_refresh = false;
};

SystemUi::SystemUi(runtime::GraphicsHost &graphics,
                   apps::AppRepository &repository)
    : impl_(std::make_unique<Impl>(graphics, repository)) {}

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
