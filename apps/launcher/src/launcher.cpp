#include "oos/apps/launcher/launcher.h"

#include "oos/apps/app_repository.h"
#include "oos/apps/launcher/logo.h"
#include "oos/sdk/ui/fonts.h"
#include "oos/sdk/ui/icons.h"
#include "oos/sdk/ui/lvgl_backend.h"
#include "oos/sdk/ui/theme.h"
#include "oos/ui/status_bar_appearance.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <utility>

namespace oos::apps::launcher {
namespace {

constexpr uint16_t kKeyUp = 103;
constexpr uint16_t kKeyLeft = 105;
constexpr uint16_t kKeyRight = 106;
constexpr uint16_t kKeyDown = 108;
constexpr uint16_t kKeySoftLeft = 139;
constexpr uint16_t kKeyBack = 158;
constexpr uint16_t kKeyOk = 352;
constexpr uint16_t kKeySoftRight = 357;

constexpr uint32_t kOrange = sdk::ui::theme::kPrimary;
constexpr uint32_t kOrangeLight = sdk::ui::theme::kPrimaryLight;
constexpr uint32_t kCanvas = sdk::ui::theme::kCanvas;
constexpr uint32_t kSurface = sdk::ui::theme::kSurface;
constexpr uint32_t kSelectedSurface = sdk::ui::theme::kSelectedSurface;
constexpr uint32_t kText = sdk::ui::theme::kText;
constexpr uint32_t kMuted = sdk::ui::theme::kMuted;
constexpr uint32_t kDivider = sdk::ui::theme::kDivider;
constexpr uint32_t kWhite = sdk::ui::theme::kWhite;

struct AppVisual {
  const char *name;
  const char *symbol;
  uint32_t icon_color;
};

constexpr std::array<AppVisual, 6> kCoreApps = {
    {{"Phone", sdk::ui::icons::kPhone, 0x2f7658},
     {"Messages", sdk::ui::icons::kMessages, 0x35669a},
     {"Contacts", sdk::ui::icons::kContacts, 0x6e527e},
     {"Camera", sdk::ui::icons::kCamera, 0x9b493b},
     {"Files", sdk::ui::icons::kFiles, 0x397174},
     {"Settings", sdk::ui::icons::kSettings, 0x625e59}}};

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

void constrainLabel(lv_obj_t *label, int width, lv_text_align_t alignment) {
  const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
  lv_obj_set_size(label, width, lv_font_get_line_height(font));
  lv_obj_set_style_text_align(label, alignment, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
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

class Launcher::Impl {
public:
  enum class View { Home, Apps };

  Impl(runtime::GraphicsHost &graphics, AppRepository &repository,
       ui::StatusBarAppearanceController &status_bar)
      : backend(graphics), repository(repository), status_bar(status_bar) {}

  bool initialize() {
    if (initialized)
      return true;
    if (!backend.initialize()) {
      error = backend.lastError();
      return false;
    }
    root = backend.root();
    if (!root) {
      error = "Launcher did not create an LVGL root";
      backend.shutdown();
      return false;
    }
    stripObject(root);
    lv_obj_set_style_bg_color(root, color(kCanvas), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    content = lv_obj_create(root);
    stripObject(content);
    lv_obj_set_size(content, LV_PCT(100), 272);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 0);
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
    soft_left = makeLabel(softkeys, "", sdk::ui::fonts::get(12), kText);
    soft_center =
        makeLabel(softkeys, "", sdk::ui::fonts::get(12), kOrangeLight);
    soft_right = makeLabel(softkeys, "", sdk::ui::fonts::get(12), kText);
    constrainLabel(soft_left, 72, LV_TEXT_ALIGN_LEFT);
    constrainLabel(soft_center, 80, LV_TEXT_ALIGN_CENTER);
    constrainLabel(soft_right, 72, LV_TEXT_ALIGN_RIGHT);
    lv_obj_align(soft_left, LV_ALIGN_LEFT_MID, 7, 0);
    lv_obj_align(soft_center, LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(soft_right, LV_ALIGN_RIGHT_MID, -7, 0);

    showHome();
    initialized = true;
    error.clear();
    return true;
  }

  void shutdown() {
    backend.shutdown();
    root = nullptr;
    content = nullptr;
    softkeys = nullptr;
    soft_left = nullptr;
    soft_center = nullptr;
    soft_right = nullptr;
    home_time = nullptr;
    home_date = nullptr;
    notice = nullptr;
    app_cards.fill(nullptr);
    initialized = false;
    needs_refresh = false;
    notice_until_us = 0;
    last_minute = -1;
  }

  void showHome() {
    status_bar.setStatusBarAppearance({kCanvas, false});
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
        makeLabel(content, "Orange OS", sdk::ui::fonts::get(14), kText);
    lv_obj_set_pos(name, 59, 19);
    lv_obj_t *system =
        makeLabel(content, "SYSTEM", sdk::ui::fonts::get(12), kOrange);
    lv_obj_set_pos(system, 59, 38);

    home_time = makeLabel(content, currentTime().c_str(),
                          sdk::ui::fonts::get(36), kText);
    lv_obj_set_pos(home_time, 13, 77);
    home_date = makeLabel(content, currentDate().c_str(),
                          sdk::ui::fonts::get(12), kMuted);
    lv_obj_set_pos(home_date, 16, 121);
    lv_label_set_text(soft_left, "Notices");
    lv_label_set_text(soft_center, "Apps");
    lv_label_set_text(soft_right, "Camera");
    updateClock();
    needs_refresh = true;
  }

  void showApps() {
    status_bar.setStatusBarAppearance({kCanvas, false});
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
      lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);

      lv_obj_t *icon = lv_obj_create(card);
      stripObject(icon);
      lv_obj_set_size(icon, 38, 38);
      lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 10);
      lv_obj_set_style_bg_color(icon, color(app.icon_color), 0);
      lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
      lv_obj_t *symbol =
          makeLabel(icon, app.symbol, sdk::ui::fonts::get(20), kWhite);
      lv_obj_align(symbol, LV_ALIGN_CENTER, 0, 0);

      lv_obj_t *label =
          makeLabel(card, app.name, sdk::ui::fonts::get(12), kText);
      constrainLabel(label, 68, LV_TEXT_ALIGN_CENTER);
      lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -10);
      app_cards[index] = card;
    }
    updateSelection();
    lv_label_set_text(soft_left, "Options");
    lv_label_set_text(soft_center, "Open");
    lv_label_set_text(soft_right, "Back");
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
    }
    needs_refresh = true;
  }

  void updateClock() {
    const int64_t minute = static_cast<int64_t>(std::time(nullptr) / 60);
    if (minute == last_minute)
      return;
    last_minute = minute;
    if (home_time)
      lv_label_set_text(home_time, currentTime().c_str());
    if (home_date)
      lv_label_set_text(home_date, currentDate().c_str());
    needs_refresh = true;
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
    lv_obj_t *label =
        makeLabel(notice, message, sdk::ui::fonts::get(12), kText);
    constrainLabel(label, 204, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_to_index(notice, -1);
    notice_until_us = monotonic_us + 1400000;
    needs_refresh = true;
  }

  bool dispatchKey(const input::KeyEvent &event, int64_t monotonic_us) {
    if (!initialized)
      return false;
    backend.dispatchKey(event);
    if (event.action == input::KeyAction::Released)
      return true;
    if (view == View::Home) {
      if (event.code == kKeyOk)
        showApps();
      else if (event.code == kKeySoftLeft)
        showNotice("No notifications", monotonic_us);
      else if (event.code == kKeySoftRight)
        showNotice("Camera is not installed", monotonic_us);
      return true;
    }
    switch (event.code) {
    case kKeyBack:
    case kKeySoftRight:
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
      if (selected == kCoreApps.size() - 1)
        launch_request = "cc.jaxy.oos.settings";
      else
        showNotice("App is not installed", monotonic_us);
      break;
    case kKeySoftLeft:
      showNotice("Options are not available", monotonic_us);
      break;
    default:
      break;
    }
    return true;
  }

  bool frame(int64_t monotonic_us, uint32_t &next_delay_ms) {
    if (!initialized)
      return false;
    updateClock();
    if (notice && monotonic_us >= notice_until_us) {
      lv_obj_delete(notice);
      notice = nullptr;
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
    const uint32_t seconds = static_cast<uint32_t>(std::time(nullptr) % 60);
    next_delay_ms = std::max(1u, (60u - seconds) * 1000u);
    if (notice && notice_until_us > monotonic_us)
      next_delay_ms = std::min(
          next_delay_ms,
          static_cast<uint32_t>((notice_until_us - monotonic_us + 999) / 1000));
    return true;
  }

  sdk::ui::LvglBackend backend;
  AppRepository &repository;
  ui::StatusBarAppearanceController &status_bar;
  lv_obj_t *root = nullptr;
  lv_obj_t *content = nullptr;
  lv_obj_t *softkeys = nullptr;
  lv_obj_t *soft_left = nullptr;
  lv_obj_t *soft_center = nullptr;
  lv_obj_t *soft_right = nullptr;
  lv_obj_t *home_time = nullptr;
  lv_obj_t *home_date = nullptr;
  lv_obj_t *notice = nullptr;
  std::array<lv_obj_t *, 6> app_cards = {};
  std::string error;
  std::string launch_request;
  int64_t notice_until_us = 0;
  int64_t last_minute = -1;
  size_t selected = 0;
  View view = View::Home;
  bool initialized = false;
  bool needs_refresh = false;
};

Launcher::Launcher(runtime::GraphicsHost &graphics, AppRepository &repository,
                   ui::StatusBarAppearanceController &status_bar)
    : impl_(std::make_unique<Impl>(graphics, repository, status_bar)) {}

Launcher::~Launcher() { shutdown(); }

bool Launcher::initialize() { return impl_->initialize(); }

void Launcher::shutdown() {
  if (impl_)
    impl_->shutdown();
}

bool Launcher::dispatchKey(const input::KeyEvent &event, int64_t monotonic_us) {
  return impl_->dispatchKey(event, monotonic_us);
}

bool Launcher::frame(int64_t monotonic_us, uint32_t &next_delay_ms) {
  return impl_->frame(monotonic_us, next_delay_ms);
}

const char *Launcher::lastError() const { return impl_->error.c_str(); }

std::string Launcher::takeLaunchRequest() {
  std::string request = std::move(impl_->launch_request);
  impl_->launch_request.clear();
  return request;
}

} // namespace oos::apps::launcher
