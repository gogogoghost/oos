#include "oos/apps/settings/settings.h"

#include "oos/apps/app_repository.h"
#include "oos/device/device.h"
#include "oos/device/service_provider.h"
#include "oos/network/wifi_manager.h"
#include "oos/sdk/ui/fonts.h"
#include "oos/sdk/ui/icons.h"
#include "oos/sdk/ui/lvgl_backend.h"
#include "oos/sdk/ui/theme.h"
#include "oos/ui/status_bar_appearance.h"
#include "oos/ui/system_ui_settings.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <thread>
#include <utility>
#include <vector>

namespace oos::apps::settings {
namespace {

constexpr uint16_t kKeyUp = 103;
constexpr uint16_t kKeyDown = 108;
constexpr uint16_t kKeySoftLeft = 139;
constexpr uint16_t kKeyBack = 158;
constexpr uint16_t kKeyOk = 352;
constexpr uint16_t kKeySoftRight = 357;
constexpr uint16_t kKeyStar = 522;
constexpr uint16_t kKeyPound = 523;

constexpr uint32_t kOrange = sdk::ui::theme::kPrimary;
constexpr uint32_t kOrangeLight = sdk::ui::theme::kPrimaryLight;
constexpr uint32_t kCanvas = sdk::ui::theme::kCanvas;
constexpr uint32_t kSurface = sdk::ui::theme::kSurface;
constexpr uint32_t kSelectedSurface = sdk::ui::theme::kSelectedSurface;
constexpr uint32_t kText = sdk::ui::theme::kText;
constexpr uint32_t kMuted = sdk::ui::theme::kMuted;
constexpr uint32_t kDivider = sdk::ui::theme::kDivider;
constexpr uint32_t kWhite = sdk::ui::theme::kWhite;
constexpr int kListViewportHeight = 238;

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

std::string formatBytes(uint64_t bytes) {
  constexpr uint64_t kMiB = 1024 * 1024;
  constexpr uint64_t kGiB = 1024 * kMiB;
  char text[32] = {};
  if (bytes >= kGiB)
    std::snprintf(text, sizeof(text), "%.1f GB",
                  static_cast<double>(bytes) / kGiB);
  else
    std::snprintf(text, sizeof(text), "%.1f MB",
                  static_cast<double>(bytes) / kMiB);
  return text;
}

std::string fileSize(const std::string &path) {
  struct stat status = {};
  return stat(path.c_str(), &status) == 0 && status.st_size >= 0
             ? formatBytes(static_cast<uint64_t>(status.st_size))
             : "Unknown";
}

} // namespace

class Settings::Impl {
public:
  static constexpr size_t kCurrentWifiItem = static_cast<size_t>(-1);

  enum class View {
    Main,
    Placeholder,
    Wifi,
    WifiNetworks,
    WifiDetails,
    WifiConfirmForget,
    WifiPassword,
    Applications,
    ApplicationDetails,
    ConfirmUninstall,
    Storage,
    DeviceInformation,
    StatusBar,
    Count,
  };

  enum class Action {
    None,
    Wifi,
    WifiToggle,
    WifiScan,
    WifiNetwork,
    WifiSavedNetwork,
    Bluetooth,
    Sim,
    Applications,
    Storage,
    DeviceInformation,
    StatusBar,
    Application,
    ToggleClock,
    ToggleNetwork,
    ToggleBattery,
  };

  struct Row {
    Action action = Action::None;
    size_t data_index = 0;
    int top = 0;
    lv_obj_t *object = nullptr;
  };

  struct RebuildState {
    size_t selected = 0;
    int scroll_y = 0;
    bool valid = false;
  };

  struct ManagedApp {
    AppRecord record;
    bool builtin = false;
  };

  struct WifiItem {
    network::WifiAccessPoint access_point;
    int saved_id = -1;
    bool connected = false;
  };

  struct RetainedPage {
    View view = View::Main;
    lv_obj_t *content = nullptr;
    lv_obj_t *list = nullptr;
    lv_obj_t *toast = nullptr;
    lv_obj_t *wifi_password_label = nullptr;
    lv_obj_t *wifi_mode_label = nullptr;
    std::vector<Row> rows;
    std::vector<ManagedApp> managed_apps;
    std::vector<network::WifiNetwork> wifi_saved;
    std::vector<network::WifiAccessPoint> wifi_access_points;
    std::vector<WifiItem> wifi_items;
    network::WifiStatus wifi_status;
    std::string wifi_password;
    std::string soft_left;
    std::string soft_center;
    std::string soft_right;
    int64_t toast_until_us = 0;
    int64_t wifi_last_key_time = 0;
    size_t selected = 0;
    size_t selected_app = 0;
    size_t selected_wifi = 0;
    size_t wifi_character_index = 0;
    int information_scroll = 0;
    int information_scroll_max = 0;
    int wifi_details_network_id = -1;
    int wifi_input_mode = 0;
    uint16_t wifi_last_key = 0;
    bool wifi_enabled = false;
    bool wifi_details_connected = false;
  };

  enum class WifiTask {
    None,
    Refresh,
    Scan,
    Enable,
    Disable,
    Connect,
    Select,
    Disconnect,
    Forget,
  };

  struct WifiTaskResult {
    WifiTask task = WifiTask::None;
    bool success = false;
    bool enabled = false;
    network::WifiStatus status;
    std::vector<network::WifiNetwork> saved;
    std::vector<network::WifiAccessPoint> access_points;
    std::string error;
  };

  Impl(runtime::GraphicsHost &graphics, AppRepository &repository,
       const device::Device &device, ui::SystemUiSettings &system,
       ui::StatusBarAppearanceController &status_bar, std::string data_root)
      : backend(graphics), repository(repository),
        descriptor(device.descriptor()), system(system), services(device),
        status_bar(status_bar), data_root(std::move(data_root)) {}

  bool initialize() {
    if (initialized)
      return true;
    if (!backend.initialize()) {
      error = backend.lastError();
      return false;
    }
    root = backend.root();
    if (!root) {
      error = "Settings did not create an LVGL root";
      backend.shutdown();
      return false;
    }
    stripObject(root);
    lv_obj_set_style_bg_color(root, color(kCanvas), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    content_host = lv_obj_create(root);
    stripObject(content_host);
    lv_obj_set_size(content_host, LV_PCT(100), 272);
    lv_obj_align(content_host, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(content_host, color(kCanvas), 0);
    lv_obj_set_style_bg_opa(content_host, LV_OPA_COVER, 0);

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

    initialized = true;
    showMain();
    error.clear();
    return true;
  }

  void shutdown() {
    if (wifi_worker.joinable())
      wifi_worker.join();
    backend.shutdown();
    root = nullptr;
    content_host = nullptr;
    content = nullptr;
    softkeys = nullptr;
    soft_left = nullptr;
    soft_center = nullptr;
    soft_right = nullptr;
    list = nullptr;
    toast = nullptr;
    wifi_password_label = nullptr;
    wifi_mode_label = nullptr;
    rows.clear();
    navigation_stack.clear();
    managed_apps.clear();
    initialized = false;
    needs_refresh = false;
  }

  bool collectWifiState(WifiTaskResult &result, bool include_scan) {
    if (!services.wifiEnabled(result.enabled)) {
      result.error = services.lastError();
      return false;
    }
    if (!result.enabled)
      return true;
    if (!services.wifiStatus(result.status) ||
        !services.wifiListNetworks(result.saved)) {
      result.error = services.lastError();
      return false;
    }
    if (include_scan && !services.wifiScan(result.access_points, 1500)) {
      result.error = services.lastError();
      return false;
    }
    return true;
  }

  bool waitForWifiConnection(WifiTaskResult &result) {
    for (int attempt = 0; attempt < 40; ++attempt) {
      network::WifiStatus status;
      if (services.wifiStatus(status) && status.state == "COMPLETED" &&
          !status.ssid.empty()) {
        if (!services.ipUseDhcp(10000)) {
          result.error = services.lastError();
          return false;
        }
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    result.error = "Connection timed out";
    return false;
  }

  void
  startWifiTask(WifiTask task, std::string ssid = {},
                network::WifiSecurity security = network::WifiSecurity::Open,
                std::string credential = {}, int network_id = -1) {
    if (wifi_busy)
      return;
    if (wifi_worker.joinable())
      wifi_worker.join();
    wifi_busy = true;
    wifi_worker = std::thread([this, task, ssid = std::move(ssid), security,
                               credential = std::move(credential),
                               network_id]() mutable {
      WifiTaskResult result;
      result.task = task;
      bool success = true;
      switch (task) {
      case WifiTask::Refresh:
        break;
      case WifiTask::Scan:
        break;
      case WifiTask::Enable:
        success = services.wifiSetEnabled(true);
        if (success) {
          std::vector<network::WifiNetwork> saved;
          if (services.wifiListNetworks(saved) && !saved.empty())
            success = waitForWifiConnection(result);
        }
        break;
      case WifiTask::Disable:
        success = services.wifiSetEnabled(false);
        break;
      case WifiTask::Connect: {
        int created_id = -1;
        success =
            services.wifiConnect(ssid, security, credential, created_id) &&
            services.wifiSaveConfiguration() && waitForWifiConnection(result);
        break;
      }
      case WifiTask::Select:
        success =
            services.wifiSelect(network_id) && waitForWifiConnection(result);
        break;
      case WifiTask::Disconnect:
        success = services.wifiDisconnect();
        break;
      case WifiTask::Forget:
        success =
            services.wifiForget(network_id) && services.wifiSaveConfiguration();
        break;
      case WifiTask::None:
        success = false;
        break;
      }
      if (!success && result.error.empty())
        result.error = services.lastError();
      if (success)
        success = collectWifiState(result, task == WifiTask::Scan);
      result.success = success;
      std::lock_guard<std::mutex> lock(wifi_mutex);
      wifi_result = std::move(result);
      wifi_result_ready = true;
    });
  }

  void setSoftkeys(const char *left, const char *center, const char *right) {
    lv_label_set_text(soft_left, left);
    lv_label_set_text(soft_center, center);
    lv_label_set_text(soft_right, right);
  }

  static std::string labelText(lv_obj_t *label) {
    return label ? lv_label_get_text(label) : "";
  }

  void resetPageObjects() {
    content = nullptr;
    list = nullptr;
    toast = nullptr;
    wifi_password_label = nullptr;
    wifi_mode_label = nullptr;
    rows.clear();
    selected = 0;
    information_scroll = 0;
    information_scroll_max = 0;
    toast_until_us = 0;
  }

  void retainCurrentPage() {
    if (!content || !has_rendered_view)
      return;
    RetainedPage page;
    page.view = rendered_view;
    page.content = content;
    page.list = list;
    page.toast = toast;
    page.wifi_password_label = wifi_password_label;
    page.wifi_mode_label = wifi_mode_label;
    page.rows = rows;
    page.managed_apps = managed_apps;
    page.wifi_saved = wifi_saved;
    page.wifi_access_points = wifi_access_points;
    page.wifi_items = wifi_items;
    page.wifi_status = wifi_status;
    page.wifi_password = wifi_password;
    page.soft_left = labelText(soft_left);
    page.soft_center = labelText(soft_center);
    page.soft_right = labelText(soft_right);
    page.toast_until_us = toast_until_us;
    page.wifi_last_key_time = wifi_last_key_time;
    page.selected = selected;
    page.selected_app = selected_app;
    page.selected_wifi = selected_wifi;
    page.wifi_character_index = wifi_character_index;
    page.information_scroll = information_scroll;
    page.information_scroll_max = information_scroll_max;
    page.wifi_details_network_id = wifi_details_network_id;
    page.wifi_input_mode = wifi_input_mode;
    page.wifi_last_key = wifi_last_key;
    page.wifi_enabled = wifi_enabled;
    page.wifi_details_connected = wifi_details_connected;
    lv_obj_add_flag(content, LV_OBJ_FLAG_HIDDEN);
    navigation_stack.push_back(std::move(page));
    resetPageObjects();
  }

  void restorePage(RetainedPage page) {
    view = page.view;
    rendered_view = page.view;
    has_rendered_view = true;
    content = page.content;
    list = page.list;
    toast = page.toast;
    wifi_password_label = page.wifi_password_label;
    wifi_mode_label = page.wifi_mode_label;
    rows = std::move(page.rows);
    managed_apps = std::move(page.managed_apps);
    wifi_saved = std::move(page.wifi_saved);
    wifi_access_points = std::move(page.wifi_access_points);
    wifi_items = std::move(page.wifi_items);
    wifi_status = std::move(page.wifi_status);
    wifi_password = std::move(page.wifi_password);
    toast_until_us = page.toast_until_us;
    wifi_last_key_time = page.wifi_last_key_time;
    selected = page.selected;
    selected_app = page.selected_app;
    selected_wifi = page.selected_wifi;
    wifi_character_index = page.wifi_character_index;
    information_scroll = page.information_scroll;
    information_scroll_max = page.information_scroll_max;
    wifi_details_network_id = page.wifi_details_network_id;
    wifi_input_mode = page.wifi_input_mode;
    wifi_last_key = page.wifi_last_key;
    wifi_enabled = page.wifi_enabled;
    wifi_details_connected = page.wifi_details_connected;
    lv_obj_remove_flag(content, LV_OBJ_FLAG_HIDDEN);
    setSoftkeys(page.soft_left.c_str(), page.soft_center.c_str(),
                page.soft_right.c_str());
    needs_refresh = true;
  }

  bool popPage() {
    if (navigation_stack.empty())
      return false;
    if (content)
      lv_obj_delete(content);
    resetPageObjects();
    RetainedPage page = std::move(navigation_stack.back());
    navigation_stack.pop_back();
    restorePage(std::move(page));
    return true;
  }

  bool popToView(View target) {
    while (has_rendered_view && rendered_view != target) {
      if (!popPage())
        return false;
    }
    return has_rendered_view && rendered_view == target;
  }

  void beginPage(const char *title) {
    status_bar.setStatusBarAppearance({kSurface, false});
    rebuild_state = {};
    if (content && has_rendered_view && rendered_view == view) {
      rebuild_state.selected = selected;
      rebuild_state.scroll_y = list ? lv_obj_get_scroll_y(list) : 0;
      rebuild_state.valid = true;
      lv_obj_delete(content);
      resetPageObjects();
    } else if (content) {
      retainCurrentPage();
    }
    rendered_view = view;
    has_rendered_view = true;

    content = lv_obj_create(content_host);
    stripObject(content);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(content, 0, 0);
    lv_obj_set_style_bg_color(content, color(kCanvas), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);

    lv_obj_t *header = lv_obj_create(content);
    stripObject(header);
    lv_obj_set_size(header, LV_PCT(100), 34);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, color(kSurface), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, color(kDivider), 0);
    lv_obj_t *heading =
        makeLabel(header, title, sdk::ui::fonts::get(14), kText);
    lv_obj_align(heading, LV_ALIGN_LEFT_MID, 10, 0);

    list = lv_obj_create(content);
    lv_obj_set_size(list, LV_PCT(100), kListViewportHeight);
    lv_obj_set_pos(list, 0, 34);
    lv_obj_set_style_bg_color(list, color(kCanvas), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  }

  void addSection(const char *name, int &y) {
    lv_obj_t *section = lv_obj_create(list);
    stripObject(section);
    lv_obj_set_size(section, 240, 22);
    lv_obj_set_pos(section, 0, y);
    lv_obj_set_style_bg_color(section, color(kCanvas), 0);
    lv_obj_set_style_bg_opa(section, LV_OPA_COVER, 0);
    lv_obj_t *label =
        makeLabel(section, name, sdk::ui::fonts::get(12), kOrangeLight);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 1);
    y += 22;
  }

  void addRow(Action action, const char *title, const char *subtitle,
              const char *symbol, int &y, size_t data_index = 0,
              const char *value = nullptr, bool status_dot = false) {
    lv_obj_t *row = lv_obj_create(list);
    stripObject(row);
    lv_obj_set_size(row, 240, 48);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_bg_color(row, color(kCanvas), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_t *icon = lv_obj_create(row);
    stripObject(icon);
    lv_obj_set_size(icon, 30, 30);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 9, 0);
    lv_obj_set_style_bg_color(icon, color(kSurface), 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_t *glyph = makeLabel(icon, symbol, sdk::ui::fonts::get(14), kWhite);
    lv_obj_align(glyph, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *name = makeLabel(row, title, sdk::ui::fonts::get(12), kText);
    const int text_width = value || status_dot ? 116 : 158;
    constrainLabel(name, text_width, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(name, 49, subtitle && subtitle[0] ? 7 : 16);
    if (subtitle && subtitle[0]) {
      lv_obj_t *detail =
          makeLabel(row, subtitle, sdk::ui::fonts::get(12), kMuted);
      lv_obj_set_pos(detail, 49, 25);
      constrainLabel(detail, text_width, LV_TEXT_ALIGN_LEFT);
    }
    if (status_dot) {
      lv_obj_t *dot = lv_obj_create(row);
      stripObject(dot);
      lv_obj_set_size(dot, 7, 7);
      lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(dot, color(kOrange), 0);
      lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
      lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -12, 0);
    } else {
      lv_obj_t *trailing =
          makeLabel(row, value ? value : LV_SYMBOL_RIGHT,
                    sdk::ui::fonts::get(12), value ? kOrangeLight : kMuted);
      constrainLabel(trailing, value ? 62 : 16, LV_TEXT_ALIGN_RIGHT);
      lv_obj_align(trailing, LV_ALIGN_RIGHT_MID, -9, 0);
    }
    rows.push_back({action, data_index, y, row});
    y += 48;
  }

  void finishList(int) {
    lv_obj_update_layout(list);
    if (rebuild_state.valid && !rows.empty()) {
      selected = std::min(rebuild_state.selected, rows.size() - 1);
      lv_obj_scroll_to_y(
          list,
          std::clamp(rebuild_state.scroll_y, 0, lv_obj_get_scroll_bottom(list)),
          LV_ANIM_OFF);
    }
    updateSelection();
    needs_refresh = true;
  }

  void updateSelection() {
    if (rows.empty())
      return;
    selected %= rows.size();
    for (size_t index = 0; index < rows.size(); ++index) {
      const bool active = index == selected;
      lv_obj_set_style_bg_color(rows[index].object,
                                color(active ? kSelectedSurface : kCanvas), 0);
    }
    constexpr int kRowHeight = 48;
    const int scroll_y = lv_obj_get_scroll_y(list);
    const int row_top = rows[selected].top;
    const int row_bottom = row_top + kRowHeight;
    if (row_top < scroll_y)
      lv_obj_scroll_to_y(list, row_top, LV_ANIM_OFF);
    else if (row_bottom > scroll_y + kListViewportHeight)
      lv_obj_scroll_to_y(list, row_bottom - kListViewportHeight, LV_ANIM_OFF);
    needs_refresh = true;
  }

  void showMain() {
    view = View::Main;
    beginPage("Settings");
    int y = 0;
    addSection("NETWORK", y);
    addRow(Action::Wifi, "Wi-Fi", "Networks and connections",
           sdk::ui::icons::kWifi, y);
    addRow(Action::Bluetooth, "Bluetooth", "Devices and pairing",
           sdk::ui::icons::kBluetooth, y);
    addRow(Action::Sim, "SIM manager", "Mobile network and SIM",
           sdk::ui::icons::kSimCard, y);
    addSection("APPS", y);
    addRow(Action::Applications, "Manage apps", "Info and uninstall",
           sdk::ui::icons::kSettings, y);
    addSection("DEVICE", y);
    addRow(Action::Storage, "Storage", "Used and available space",
           sdk::ui::icons::kStorage, y);
    addRow(Action::DeviceInformation, "Device information",
           "Hardware and system", sdk::ui::icons::kDeviceInfo, y);
    addRow(Action::StatusBar, "Status bar", "Visible indicators",
           sdk::ui::icons::kStatusBar, y);
    finishList(y);
    setSoftkeys("", "Select", "Back");
  }

  static bool wifiSecured(const network::WifiAccessPoint &access_point) {
    return access_point.flags.find("WPA") != std::string::npos ||
           access_point.flags.find("WEP") != std::string::npos;
  }

  static bool wifiUsesWep(const network::WifiAccessPoint &access_point) {
    return access_point.flags.find("WEP") != std::string::npos;
  }

  static const char *wifiSignal(int dbm) {
    if (dbm >= -55)
      return "Excellent";
    if (dbm >= -67)
      return "Good";
    if (dbm >= -75)
      return "Fair";
    return "Weak";
  }

  int savedWifiId(const std::string &ssid) const {
    for (const network::WifiNetwork &network : wifi_saved) {
      if (network.ssid == ssid)
        return network.id;
    }
    return -1;
  }

  void rebuildWifiItems() {
    wifi_items.clear();
    for (const network::WifiAccessPoint &access_point : wifi_access_points) {
      if (access_point.ssid.empty())
        continue;
      auto existing = std::find_if(
          wifi_items.begin(), wifi_items.end(), [&](const WifiItem &item) {
            return item.access_point.ssid == access_point.ssid;
          });
      if (existing != wifi_items.end()) {
        if (access_point.signal_dbm > existing->access_point.signal_dbm)
          existing->access_point = access_point;
        continue;
      }
      WifiItem item;
      item.access_point = access_point;
      item.saved_id = savedWifiId(access_point.ssid);
      item.connected = wifi_status.state == "COMPLETED" &&
                       wifi_status.ssid == access_point.ssid;
      wifi_items.push_back(std::move(item));
    }
    std::sort(wifi_items.begin(), wifi_items.end(),
              [](const WifiItem &left, const WifiItem &right) {
                if (left.connected != right.connected)
                  return left.connected;
                if ((left.saved_id >= 0) != (right.saved_id >= 0))
                  return left.saved_id >= 0;
                return left.access_point.signal_dbm >
                       right.access_point.signal_dbm;
              });
  }

  void showWifi() {
    view = View::Wifi;
    beginPage("Wi-Fi");
    int y = 0;
    addSection("WIRELESS", y);
    const char *toggle_detail = wifi_busy      ? "Changing Wi-Fi state"
                                : wifi_enabled ? "Wireless is enabled"
                                               : "Wireless is disabled";
    addRow(Action::WifiToggle, "Wi-Fi", toggle_detail, sdk::ui::icons::kWifi, y,
           0, wifi_enabled ? "On" : "Off");
    if (wifi_enabled) {
      if (wifi_status.state == "COMPLETED" && !wifi_status.ssid.empty()) {
        addSection("CONNECTED", y);
        addRow(Action::WifiNetwork, wifi_status.ssid.c_str(),
               wifi_status.ip_address.empty() ? "Connected"
                                              : wifi_status.ip_address.c_str(),
               sdk::ui::icons::kWifi, y, kCurrentWifiItem);
      }
      addSection("NETWORKS", y);
      addRow(Action::WifiScan, "Available networks",
             wifi_busy ? "Working..." : "Scan and connect",
             sdk::ui::icons::kWifi, y);
      bool has_saved = false;
      for (const network::WifiNetwork &network : wifi_saved) {
        if (network.id != wifi_status.network_id)
          has_saved = true;
      }
      if (has_saved) {
        addSection("SAVED", y);
        for (size_t index = 0; index < wifi_saved.size(); ++index) {
          if (wifi_saved[index].id == wifi_status.network_id)
            continue;
          addRow(Action::WifiSavedNetwork, wifi_saved[index].ssid.c_str(),
                 "Saved network", sdk::ui::icons::kWifi, y, index);
        }
      }
    }
    finishList(y);
    setSoftkeys("", wifi_busy ? "" : "Select", "Back");
  }

  void showWifiNetworks() {
    view = View::WifiNetworks;
    beginPage("Available networks");
    int y = 0;
    addSection(wifi_busy ? "SCANNING..." : "NETWORKS", y);
    for (size_t index = 0; index < wifi_items.size(); ++index) {
      const WifiItem &item = wifi_items[index];
      std::string detail = wifiUsesWep(item.access_point)   ? "WEP unsupported"
                           : wifiSecured(item.access_point) ? "Secured"
                                                            : "Open";
      detail += " - ";
      detail += wifiSignal(item.access_point.signal_dbm);
      const char *state =
          item.saved_id >= 0 && !item.connected ? "Saved" : nullptr;
      addRow(Action::WifiNetwork, item.access_point.ssid.c_str(),
             detail.c_str(), sdk::ui::icons::kWifi, y, index, state,
             item.connected);
    }
    finishList(y);
    setSoftkeys(wifi_busy ? "" : "Rescan",
                wifi_busy || wifi_items.empty() ? "" : "Select", "Back");
  }

  void showWifiDetails(size_t index) {
    view = View::WifiDetails;
    beginPage("Network details");
    selected_wifi = index;
    int network_id = -1;
    std::string ssid;
    bool connected = false;
    if (index == kCurrentWifiItem) {
      network_id = wifi_status.network_id;
      ssid = wifi_status.ssid;
      connected = wifi_status.state == "COMPLETED";
    } else if (index < wifi_items.size()) {
      network_id = wifi_items[index].saved_id;
      ssid = wifi_items[index].access_point.ssid;
      connected = wifi_items[index].connected;
    }
    int y = 0;
    addInformationRow("Network", ssid, y);
    addInformationRow("State", connected ? "Connected" : "Saved", y);
    if (connected && !wifi_status.ip_address.empty())
      addInformationRow("IP address", wifi_status.ip_address, y);
    finishInformation(y);
    wifi_details_network_id = network_id;
    wifi_details_connected = connected;
    setSoftkeys(network_id >= 0 ? "Forget" : "",
                connected         ? "Disconnect"
                : network_id >= 0 ? "Connect"
                                  : "",
                "Back");
  }

  void showSavedWifiDetails(size_t index) {
    if (index >= wifi_saved.size())
      return;
    view = View::WifiDetails;
    beginPage("Network details");
    selected_wifi = kCurrentWifiItem;
    int y = 0;
    addInformationRow("Network", wifi_saved[index].ssid, y);
    addInformationRow("State", "Saved", y);
    finishInformation(y);
    wifi_details_network_id = wifi_saved[index].id;
    wifi_details_connected = false;
    setSoftkeys("Forget", "Connect", "Back");
  }

  void showConfirmWifiForget() {
    if (wifi_details_network_id < 0)
      return;
    view = View::WifiConfirmForget;
    beginPage("Forget network");
    lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *warning =
        makeLabel(content, LV_SYMBOL_WARNING, sdk::ui::fonts::get(20), kOrange);
    lv_obj_align(warning, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_t *message = makeLabel(content, "Remove saved credentials?",
                                  sdk::ui::fonts::get(12), kText);
    lv_obj_align(message, LV_ALIGN_TOP_MID, 0, 116);
    setSoftkeys("", "Forget", "Back");
    needs_refresh = true;
  }

  void showWifiPassword(size_t index) {
    if (index >= wifi_items.size())
      return;
    view = View::WifiPassword;
    beginPage("Wi-Fi password");
    selected_wifi = index;
    wifi_password.clear();
    wifi_input_mode = 0;
    wifi_last_key = 0;
    wifi_last_key_time = 0;
    lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *network =
        makeLabel(content, wifi_items[index].access_point.ssid.c_str(),
                  sdk::ui::fonts::get(14), kText);
    constrainLabel(network, 216, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(network, LV_ALIGN_TOP_LEFT, 12, 58);
    wifi_password_label =
        makeLabel(content, "", sdk::ui::fonts::get(20), kText);
    lv_obj_set_size(wifi_password_label, 216, 36);
    lv_label_set_long_mode(wifi_password_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(wifi_password_label, 12, 91);
    lv_obj_set_style_bg_color(wifi_password_label, color(kSurface), 0);
    lv_obj_set_style_bg_opa(wifi_password_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(wifi_password_label, 8, 0);
    wifi_mode_label =
        makeLabel(content, "abc", sdk::ui::fonts::get(12), kOrangeLight);
    lv_obj_align(wifi_mode_label, LV_ALIGN_TOP_RIGHT, -13, 136);
    lv_obj_t *help = makeLabel(content, "# mode   Back deletes",
                               sdk::ui::fonts::get(12), kMuted);
    constrainLabel(help, 168, LV_TEXT_ALIGN_LEFT);
    lv_obj_align(help, LV_ALIGN_TOP_LEFT, 12, 136);
    updateWifiPasswordLabels();
    setSoftkeys("Clear", "Connect", "Cancel");
    needs_refresh = true;
  }

  void updateWifiPasswordLabels() {
    if (!wifi_password_label || !wifi_mode_label)
      return;
    std::string masked(wifi_password.size(), '*');
    lv_label_set_text(wifi_password_label,
                      masked.empty() ? "Password" : masked.c_str());
    constexpr const char *modes[] = {"abc", "ABC", "123"};
    lv_label_set_text(wifi_mode_label, modes[wifi_input_mode]);
    needs_refresh = true;
  }

  void enterWifiPasswordKey(uint16_t code, int64_t monotonic_us) {
    if (code < 2 || code > 11)
      return;
    const int digit = code == 11 ? 0 : static_cast<int>(code - 1);
    constexpr const char *lower[] = {" 0",   "1.,@-_", "abc2", "def3",
                                     "ghi4", "jkl5",   "mno6", "pqrs7",
                                     "tuv8", "wxyz9"};
    constexpr const char *upper[] = {" 0",   "1.,@-_", "ABC2", "DEF3",
                                     "GHI4", "JKL5",   "MNO6", "PQRS7",
                                     "TUV8", "WXYZ9"};
    if (wifi_input_mode == 2) {
      if (wifi_password.size() < 64)
        wifi_password.push_back(static_cast<char>('0' + digit));
      wifi_last_key = 0;
    } else {
      const char *characters =
          wifi_input_mode == 0 ? lower[digit] : upper[digit];
      const size_t count = std::strlen(characters);
      if (wifi_last_key == code && !wifi_password.empty() &&
          monotonic_us - wifi_last_key_time <= 900000) {
        wifi_character_index = (wifi_character_index + 1) % count;
        wifi_password.back() = characters[wifi_character_index];
      } else if (wifi_password.size() < 64) {
        wifi_character_index = 0;
        wifi_password.push_back(characters[0]);
      }
      wifi_last_key = code;
      wifi_last_key_time = monotonic_us;
    }
    updateWifiPasswordLabels();
  }

  void showPlaceholder(Action action) {
    view = View::Placeholder;
    const char *title =
        action == Action::Bluetooth ? "Bluetooth" : "SIM manager";
    const char *symbol = action == Action::Bluetooth
                             ? sdk::ui::icons::kBluetooth
                             : sdk::ui::icons::kSimCard;
    beginPage(title);
    lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *icon =
        makeLabel(content, symbol, sdk::ui::fonts::get(20), kOrange);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_t *state =
        makeLabel(content, "Coming soon", sdk::ui::fonts::get(20), kText);
    lv_obj_align(state, LV_ALIGN_TOP_MID, 0, 116);
    lv_obj_t *detail = makeLabel(content, "Hardware manager placeholder",
                                 sdk::ui::fonts::get(12), kMuted);
    lv_obj_align(detail, LV_ALIGN_TOP_MID, 0, 144);
    setSoftkeys("", "", "Back");
    needs_refresh = true;
  }

  AppRecord builtin(const char *id, const char *name, const char *role) {
    AppRecord result;
    result.manifest.id = id;
    result.manifest.name = name;
    result.manifest.version = "0.1.0";
    result.manifest.role = role;
    return result;
  }

  void showApplications() {
    view = View::Applications;
    beginPage("Applications");
    managed_apps.clear();
    managed_apps.push_back(
        {builtin("cc.jaxy.oos.launcher", "Orange OS Launcher", "launcher"),
         true});
    managed_apps.push_back(
        {builtin("cc.jaxy.oos.settings", "Settings", "settings"), true});
    managed_apps.push_back(
        {builtin("cc.jaxy.oos.systemui", "Orange OS SystemUI", "systemui"),
         true});
    std::vector<AppRecord> installed;
    const bool list_ready = repository.list(installed);
    if (!list_ready) {
      error = repository.lastError();
    } else {
      for (AppRecord &record : installed)
        managed_apps.push_back({std::move(record), false});
    }

    int y = 0;
    addSection("INSTALLED", y);
    for (size_t index = 0; index < managed_apps.size(); ++index) {
      const ManagedApp &app = managed_apps[index];
      const std::string subtitle =
          app.builtin ? "System app" : "Version " + app.record.manifest.version;
      addRow(Action::Application, app.record.manifest.name.c_str(),
             subtitle.c_str(), LV_SYMBOL_FILE, y, index);
    }
    finishList(y);
    setSoftkeys("", "Info", "Back");
    if (!list_ready)
      showNotice("Unable to read app list", 0);
  }

  void addInformationRow(const char *name, const std::string &value, int &y) {
    lv_obj_t *row = lv_obj_create(list);
    stripObject(row);
    lv_obj_set_size(row, 240, 48);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_bg_color(row, color(kCanvas), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, color(kDivider), 0);
    lv_obj_t *label = makeLabel(row, name, sdk::ui::fonts::get(12), kMuted);
    constrainLabel(label, 218, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(label, 10, 5);
    lv_obj_t *data =
        makeLabel(row, value.c_str(), sdk::ui::fonts::get(12), kText);
    constrainLabel(data, 218, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(data, 10, 27);
    y += 48;
  }

  void finishInformation(int height) {
    lv_obj_update_layout(list);
    information_scroll_max = std::max(0, height - kListViewportHeight);
    if (rebuild_state.valid) {
      information_scroll =
          std::clamp(rebuild_state.scroll_y, 0, information_scroll_max);
      lv_obj_scroll_to_y(list, information_scroll, LV_ANIM_OFF);
    }
    needs_refresh = true;
  }

  void showApplicationDetails(size_t index) {
    if (index >= managed_apps.size())
      return;
    view = View::ApplicationDetails;
    const ManagedApp &app = managed_apps[index];
    beginPage("App information");
    selected_app = index;
    int y = 0;
    addInformationRow("Name", app.record.manifest.name, y);
    addInformationRow("Application ID", app.record.manifest.id, y);
    addInformationRow("Version", app.record.manifest.version, y);
    addInformationRow(
        "Type", app.builtin ? "Built-in native app" : "WAMR application", y);
    addInformationRow("Package size",
                      app.builtin ? "Compiled into OOS"
                                  : fileSize(app.record.package_path),
                      y);
    finishInformation(y);
    setSoftkeys(app.builtin ? "" : "Uninstall", "", "Back");
  }

  void showConfirmUninstall() {
    if (selected_app >= managed_apps.size() ||
        managed_apps[selected_app].builtin)
      return;
    view = View::ConfirmUninstall;
    const ManagedApp &app = managed_apps[selected_app];
    beginPage("Uninstall app");
    lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *warning =
        makeLabel(content, LV_SYMBOL_WARNING, sdk::ui::fonts::get(20), kOrange);
    lv_obj_align(warning, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_t *name = makeLabel(content, app.record.manifest.name.c_str(),
                               sdk::ui::fonts::get(20), kText);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_t *message = makeLabel(content, "Remove app and its data?",
                                  sdk::ui::fonts::get(12), kMuted);
    lv_obj_align(message, LV_ALIGN_TOP_MID, 0, 142);
    setSoftkeys("", "Remove", "Back");
    needs_refresh = true;
  }

  void uninstallSelected() {
    if (selected_app >= managed_apps.size() ||
        managed_apps[selected_app].builtin)
      return;
    const std::string id = managed_apps[selected_app].record.manifest.id;
    if (!repository.uninstall(id.c_str())) {
      error = repository.lastError();
      popPage();
      showNotice("Uninstall failed", 0);
      return;
    }
    popToView(View::Applications);
    showApplications();
    showNotice("App uninstalled", 0);
  }

  void showStorage() {
    view = View::Storage;
    beginPage("Storage");
    struct statvfs status = {};
    int y = 0;
    if (statvfs(data_root.c_str(), &status) == 0) {
      const uint64_t total = static_cast<uint64_t>(status.f_blocks) *
                             static_cast<uint64_t>(status.f_frsize);
      const uint64_t free = static_cast<uint64_t>(status.f_bavail) *
                            static_cast<uint64_t>(status.f_frsize);
      addInformationRow("Total", formatBytes(total), y);
      addInformationRow("Used", formatBytes(total - free), y);
      addInformationRow("Available", formatBytes(free), y);
      const int percent =
          total ? static_cast<int>((total - free) * 100 / total) : 0;
      addInformationRow("Usage", std::to_string(percent) + "%", y);
    } else {
      addInformationRow("Storage", "Unavailable", y);
    }
    finishInformation(y);
    setSoftkeys("", "", "Back");
  }

  void showDeviceInformation() {
    view = View::DeviceInformation;
    beginPage("Device information");
    int y = 0;
    addInformationRow("Orange OS", "0.1.0-dev", y);
    addInformationRow("Manufacturer", descriptor.manufacturer, y);
    addInformationRow("Model", descriptor.model, y);
    addInformationRow("Device ID", descriptor.id, y);
    addInformationRow("Android API", std::to_string(descriptor.android_api), y);
    addInformationRow("Primary screen",
                      std::to_string(descriptor.primary_width) + " x " +
                          std::to_string(descriptor.primary_height),
                      y);
    const std::string secondary =
        descriptor.secondary_width && descriptor.secondary_height
            ? std::to_string(descriptor.secondary_width) + " x " +
                  std::to_string(descriptor.secondary_height)
            : "Not present";
    addInformationRow("Secondary screen", secondary, y);
    finishInformation(y);
    setSoftkeys("", "", "Back");
  }

  void showStatusBar(size_t selection = 0) {
    view = View::StatusBar;
    const ui::StatusBarPreferences preferences = system.statusBar();
    beginPage("Status bar");
    int y = 0;
    addSection("VISIBLE ITEMS", y);
    addRow(Action::ToggleClock, "Clock", "Show current time", LV_SYMBOL_HOME, y,
           0, preferences.show_clock ? "On" : "Off");
    addRow(Action::ToggleNetwork, "Network indicators",
           "Signal, radio and Wi-Fi", sdk::ui::icons::kWifi, y, 0,
           preferences.show_network ? "On" : "Off");
    addRow(Action::ToggleBattery, "Battery percentage",
           "Show numeric charge level", sdk::ui::icons::kBatteryFull, y, 0,
           preferences.show_battery_percentage ? "On" : "Off");
    selected = std::min(selection, rows.size() - 1);
    finishList(y);
    setSoftkeys("", "Toggle", "Back");
  }

  void toggleStatus(Action action) {
    ui::StatusBarPreferences preferences = system.statusBar();
    if (action == Action::ToggleClock)
      preferences.show_clock = !preferences.show_clock;
    else if (action == Action::ToggleNetwork)
      preferences.show_network = !preferences.show_network;
    else if (action == Action::ToggleBattery)
      preferences.show_battery_percentage =
          !preferences.show_battery_percentage;
    if (!system.setStatusBar(preferences.show_clock, preferences.show_network,
                             preferences.show_battery_percentage)) {
      error = system.lastError();
      showNotice("Unable to save setting", 0);
      return;
    }
    const size_t selection = selected;
    showStatusBar(selection);
  }

  void showNotice(const char *message, int64_t monotonic_us) {
    if (toast)
      lv_obj_delete(toast);
    toast = lv_obj_create(content);
    stripObject(toast);
    lv_obj_set_size(toast, 220, 30);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_color(toast, color(kSurface), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(toast, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(toast, 3, 0);
    lv_obj_set_style_border_color(toast, color(kOrange), 0);
    lv_obj_t *label = makeLabel(toast, message, sdk::ui::fonts::get(12), kText);
    constrainLabel(label, 200, LV_TEXT_ALIGN_CENTER);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_to_index(toast, -1);
    toast_until_us = monotonic_us ? monotonic_us + 1800000 : 0;
    needs_refresh = true;
  }

  void openSelected() {
    if (rows.empty())
      return;
    const Row row = rows[selected];
    switch (row.action) {
    case Action::Wifi:
      startWifiTask(WifiTask::Refresh);
      showWifi();
      break;
    case Action::WifiToggle:
      startWifiTask(wifi_enabled ? WifiTask::Disable : WifiTask::Enable);
      showWifi();
      break;
    case Action::WifiScan:
      wifi_access_points.clear();
      wifi_items.clear();
      startWifiTask(WifiTask::Scan);
      showWifiNetworks();
      break;
    case Action::WifiNetwork:
      if (wifi_busy)
        break;
      if (row.data_index == kCurrentWifiItem) {
        showWifiDetails(kCurrentWifiItem);
      } else if (row.data_index < wifi_items.size()) {
        const WifiItem &item = wifi_items[row.data_index];
        if (item.connected || item.saved_id >= 0) {
          showWifiDetails(row.data_index);
        } else if (wifiUsesWep(item.access_point)) {
          showNotice("WEP networks are not supported", 0);
        } else if (wifiSecured(item.access_point)) {
          showWifiPassword(row.data_index);
        } else {
          startWifiTask(WifiTask::Connect, item.access_point.ssid,
                        network::WifiSecurity::Open);
          showWifiNetworks();
        }
      }
      break;
    case Action::WifiSavedNetwork:
      showSavedWifiDetails(row.data_index);
      break;
    case Action::Bluetooth:
    case Action::Sim:
      showPlaceholder(row.action);
      break;
    case Action::Applications:
      showApplications();
      break;
    case Action::Storage:
      showStorage();
      break;
    case Action::DeviceInformation:
      showDeviceInformation();
      break;
    case Action::StatusBar:
      showStatusBar();
      break;
    case Action::Application:
      showApplicationDetails(row.data_index);
      break;
    case Action::ToggleClock:
    case Action::ToggleNetwork:
    case Action::ToggleBattery:
      toggleStatus(row.action);
      break;
    case Action::None:
      break;
    }
  }

  void goBack() {
    if (!popPage())
      launch_request = "cc.jaxy.oos.launcher";
  }

  bool dispatchKey(const input::KeyEvent &event, int64_t monotonic_us) {
    if (!initialized)
      return false;
    backend.dispatchKey(event);
    if (event.action == input::KeyAction::Released)
      return true;
    if (view == View::WifiPassword) {
      if (event.action == input::KeyAction::Repeated)
        return true;
      if (event.code == kKeySoftRight) {
        goBack();
      } else if (event.code == kKeySoftLeft) {
        wifi_password.clear();
        wifi_last_key = 0;
        updateWifiPasswordLabels();
      } else if (event.code == kKeyBack) {
        if (!wifi_password.empty())
          wifi_password.pop_back();
        wifi_last_key = 0;
        updateWifiPasswordLabels();
      } else if (event.code == kKeyPound) {
        wifi_input_mode = (wifi_input_mode + 1) % 3;
        wifi_last_key = 0;
        updateWifiPasswordLabels();
      } else if (event.code == kKeyStar) {
        if (wifi_password.size() < 64)
          wifi_password.push_back('*');
        wifi_last_key = 0;
        updateWifiPasswordLabels();
      } else if (event.code == kKeyOk) {
        if (wifi_password.size() < 8) {
          showNotice("Password needs at least 8 characters", monotonic_us);
        } else if (selected_wifi < wifi_items.size()) {
          const std::string ssid = wifi_items[selected_wifi].access_point.ssid;
          startWifiTask(WifiTask::Connect, ssid, network::WifiSecurity::WpaPsk,
                        wifi_password);
          wifi_password.clear();
          goBack();
        }
      } else {
        enterWifiPasswordKey(event.code, monotonic_us);
      }
      return true;
    }
    if (event.code == kKeyBack || event.code == kKeySoftRight) {
      goBack();
      return true;
    }
    if (view == View::WifiConfirmForget && event.code == kKeyOk && !wifi_busy) {
      startWifiTask(WifiTask::Forget, {}, network::WifiSecurity::Open, {},
                    wifi_details_network_id);
      popToView(View::Wifi);
      return true;
    }
    if (view == View::WifiNetworks && event.code == kKeySoftLeft &&
        !wifi_busy) {
      wifi_access_points.clear();
      wifi_items.clear();
      startWifiTask(WifiTask::Scan);
      showWifiNetworks();
      return true;
    }
    if (view == View::WifiDetails && !wifi_busy) {
      if (event.code == kKeySoftLeft && wifi_details_network_id >= 0) {
        showConfirmWifiForget();
        return true;
      }
      if (event.code == kKeyOk) {
        startWifiTask(
            wifi_details_connected ? WifiTask::Disconnect : WifiTask::Select,
            {}, network::WifiSecurity::Open, {}, wifi_details_network_id);
        showWifiDetails(selected_wifi);
        return true;
      }
    }
    if (view == View::ApplicationDetails && event.code == kKeySoftLeft) {
      showConfirmUninstall();
      return true;
    }
    if (view == View::ConfirmUninstall && event.code == kKeyOk) {
      uninstallSelected();
      if (toast && toast_until_us == 0)
        toast_until_us = monotonic_us + 1800000;
      return true;
    }
    if (rows.empty()) {
      if (list && !lv_obj_has_flag(list, LV_OBJ_FLAG_HIDDEN) &&
          (event.code == kKeyUp || event.code == kKeyDown)) {
        const int delta = event.code == kKeyDown ? 44 : -44;
        information_scroll =
            std::clamp(information_scroll + delta, 0, information_scroll_max);
        lv_obj_scroll_to_y(list, information_scroll, LV_ANIM_OFF);
        needs_refresh = true;
      }
      return true;
    }
    if (event.code == kKeyUp) {
      selected = (selected + rows.size() - 1) % rows.size();
      updateSelection();
    } else if (event.code == kKeyDown) {
      selected = (selected + 1) % rows.size();
      updateSelection();
    } else if (event.code == kKeyOk) {
      openSelected();
    }
    return true;
  }

  bool frame(int64_t monotonic_us, uint32_t &next_delay_ms) {
    if (!initialized)
      return false;
    WifiTaskResult completed;
    bool has_wifi_result = false;
    {
      std::lock_guard<std::mutex> lock(wifi_mutex);
      if (wifi_result_ready) {
        completed = std::move(wifi_result);
        wifi_result_ready = false;
        has_wifi_result = true;
      }
    }
    if (has_wifi_result) {
      if (wifi_worker.joinable())
        wifi_worker.join();
      wifi_busy = false;
      if (view == View::WifiDetails)
        popToView(View::Wifi);
      if (completed.success) {
        wifi_enabled = completed.enabled;
        wifi_status = std::move(completed.status);
        wifi_saved = std::move(completed.saved);
        if (completed.task == WifiTask::Scan)
          wifi_access_points = std::move(completed.access_points);
        rebuildWifiItems();
      } else {
        error = completed.error;
      }
      if (view == View::WifiNetworks)
        showWifiNetworks();
      else if (view == View::Wifi)
        showWifi();
      if (completed.success) {
        const char *message =
            completed.task == WifiTask::Enable    ? "Wi-Fi enabled"
            : completed.task == WifiTask::Disable ? "Wi-Fi disabled"
            : completed.task == WifiTask::Scan    ? "Scan complete"
            : completed.task == WifiTask::Connect ||
                    completed.task == WifiTask::Select
                ? "Connected"
            : completed.task == WifiTask::Disconnect ? "Disconnected"
            : completed.task == WifiTask::Forget     ? "Network forgotten"
                                                     : nullptr;
        if (message)
          showNotice(message, monotonic_us);
      } else
        showNotice(completed.error.empty() ? "Wi-Fi operation failed"
                                           : completed.error.c_str(),
                   monotonic_us);
    }
    if (toast && toast_until_us == 0)
      toast_until_us = monotonic_us + 1800000;
    if (toast && monotonic_us >= toast_until_us) {
      lv_obj_delete(toast);
      toast = nullptr;
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
    next_delay_ms = toast || wifi_busy ? 50 : 1000;
    return true;
  }

  sdk::ui::LvglBackend backend;
  AppRepository &repository;
  const device::DeviceDescriptor &descriptor;
  ui::SystemUiSettings &system;
  device::ServiceProvider services;
  ui::StatusBarAppearanceController &status_bar;
  std::string data_root;
  lv_obj_t *root = nullptr;
  lv_obj_t *content_host = nullptr;
  lv_obj_t *content = nullptr;
  lv_obj_t *softkeys = nullptr;
  lv_obj_t *soft_left = nullptr;
  lv_obj_t *soft_center = nullptr;
  lv_obj_t *soft_right = nullptr;
  lv_obj_t *list = nullptr;
  lv_obj_t *toast = nullptr;
  lv_obj_t *wifi_password_label = nullptr;
  lv_obj_t *wifi_mode_label = nullptr;
  std::vector<Row> rows;
  std::vector<ManagedApp> managed_apps;
  std::vector<network::WifiNetwork> wifi_saved;
  std::vector<network::WifiAccessPoint> wifi_access_points;
  std::vector<WifiItem> wifi_items;
  std::vector<RetainedPage> navigation_stack;
  RebuildState rebuild_state;
  network::WifiStatus wifi_status;
  std::thread wifi_worker;
  std::mutex wifi_mutex;
  WifiTaskResult wifi_result;
  std::string launch_request;
  std::string error;
  std::string wifi_password;
  int64_t toast_until_us = 0;
  int64_t wifi_last_key_time = 0;
  size_t selected = 0;
  size_t selected_app = 0;
  size_t selected_wifi = 0;
  size_t wifi_character_index = 0;
  int information_scroll = 0;
  int information_scroll_max = 0;
  int wifi_details_network_id = -1;
  int wifi_input_mode = 0;
  uint16_t wifi_last_key = 0;
  View view = View::Main;
  View rendered_view = View::Main;
  bool wifi_enabled = false;
  bool wifi_details_connected = false;
  bool wifi_busy = false;
  bool wifi_result_ready = false;
  bool initialized = false;
  bool has_rendered_view = false;
  bool needs_refresh = false;
};

Settings::Settings(runtime::GraphicsHost &graphics, AppRepository &repository,
                   const device::Device &device, ui::SystemUiSettings &system,
                   ui::StatusBarAppearanceController &status_bar,
                   std::string data_root)
    : impl_(std::make_unique<Impl>(graphics, repository, device, system,
                                   status_bar, std::move(data_root))) {}

Settings::~Settings() { shutdown(); }

bool Settings::initialize() { return impl_->initialize(); }

void Settings::shutdown() {
  if (impl_)
    impl_->shutdown();
}

bool Settings::dispatchKey(const input::KeyEvent &event, int64_t monotonic_us) {
  return impl_->dispatchKey(event, monotonic_us);
}

bool Settings::frame(int64_t monotonic_us, uint32_t &next_delay_ms) {
  return impl_->frame(monotonic_us, next_delay_ms);
}

std::string Settings::takeLaunchRequest() {
  std::string request = std::move(impl_->launch_request);
  impl_->launch_request.clear();
  return request;
}

const char *Settings::lastError() const { return impl_->error.c_str(); }

} // namespace oos::apps::settings
