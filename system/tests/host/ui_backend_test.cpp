#include "oos/apps/app_repository.h"
#include "oos/apps/launcher/launcher.h"
#include "oos/apps/settings/settings.h"
#include "oos/apps/systemui/system_ui.h"
#include "oos/compositor/compositor.h"
#include "oos/device/device.h"
#include "oos/device/display.h"
#include "oos/device/service_provider.h"
#include "oos/runtime/application_session_manager.h"
#include "oos/runtime/graphics_host.h"
#include "oos/sdk/ui/fonts.h"
#include "oos/sdk/ui/lvgl_backend.h"
#include "oos/sdk/ui/theme.h"
#include "oos/ui/system_status.h"
#include "oos/ui/system_ui_settings.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <unordered_set>

namespace {

class FakeGraphicsHost final : public oos::device::Display {
public:
  bool initialize() override { return true; }
  bool showBootFrame(const uint16_t *) override { return true; }
  bool presentSurface(const oos::compositor::SurfaceFrame &) override {
    return false;
  }
  void refresh() override {}
  void shutdown() override {}

  uint32_t width() const override { return 240; }
  uint32_t height() const override { return 320; }
  uint32_t surfaceFormat() const override { return OOS_TEXTURE_RGB565; }
  uint32_t supportedTextureFormats() const override {
    return OOS_TEXTURE_FORMAT_MASK;
  }

  bool setTexture(uint32_t texture, uint32_t format, uint32_t, uint32_t,
                  uint32_t upload_width, uint32_t upload_height,
                  uint32_t row_stride, uint32_t flags, const uint8_t *pixels,
                  size_t pixel_bytes) override {
    const uint32_t bytes_per_pixel = oosTextureBytesPerPixel(format);
    if (texture == 0 || bytes_per_pixel == 0 || upload_width == 0 ||
        upload_height == 0 || row_stride < upload_width * bytes_per_pixel ||
        !pixels ||
        pixel_bytes < static_cast<size_t>(row_stride) * (upload_height - 1) +
                          upload_width * bytes_per_pixel)
      return false;
    if ((flags & OOS_TEXTURE_REPLACE) != 0)
      textures.insert(texture);
    else if (textures.count(texture) == 0)
      return false;
    ++texture_uploads;
    return true;
  }

  bool freeTexture(uint32_t texture) override {
    textures.erase(texture);
    return true;
  }

  bool submit(const OosGfxVertex *vertices, size_t vertex_count,
              const uint16_t *indices, size_t index_count,
              const OosGfxDrawCommand *commands, size_t command_count,
              uint32_t) override {
    if ((vertex_count != 0 && !vertices) || (index_count != 0 && !indices) ||
        (command_count != 0 && !commands))
      return false;
    for (size_t index = 0; index < command_count; ++index) {
      if (commands[index].first_index + commands[index].index_count >
              index_count ||
          textures.count(commands[index].texture) == 0)
        return false;
    }
    ++frame_submissions;
    draw_commands += command_count;
    return true;
  }

  bool glesCapabilities(OosGlesCapabilities &) override { return false; }
  bool setGlesBuffer(uint32_t, uint32_t, uint32_t, const uint8_t *,
                     size_t) override {
    return false;
  }
  bool writeGlesBuffer(uint32_t, uint32_t, const uint8_t *, size_t) override {
    return false;
  }
  bool freeGlesBuffer(uint32_t) override { return false; }
  bool setGlesShader(uint32_t, uint32_t, const char *, size_t) override {
    return false;
  }
  bool freeGlesShader(uint32_t) override { return false; }
  bool setGlesProgram(uint32_t, uint32_t, uint32_t) override { return false; }
  bool freeGlesProgram(uint32_t) override { return false; }
  int32_t glesAttributeLocation(uint32_t, const char *, size_t) override {
    return -1;
  }
  int32_t glesUniformLocation(uint32_t, const char *, size_t) override {
    return -1;
  }
  bool submitGles(const OosGlesCommand *, size_t, const uint32_t *,
                  size_t) override {
    return false;
  }

  std::unordered_set<uint32_t> textures;
  size_t texture_uploads = 0;
  size_t frame_submissions = 0;
  size_t draw_commands = 0;
};

class FakeStatusSource final : public oos::ui::SystemStatusSource {
public:
  oos::ui::SystemStatusSnapshot snapshot() const override { return value; }

  oos::ui::SystemStatusSnapshot value;
};

class FakeStatusBarAppearance final
    : public oos::ui::StatusBarAppearanceHost,
      public oos::ui::StatusBarAppearanceController {
public:
  void applyStatusBarAppearance(oos::ui::StatusBarAppearance value) override {
    appearance = value;
    ++apply_count;
  }
  void setStatusBarAppearance(oos::ui::StatusBarAppearance value) override {
    appearance = value;
    ++set_count;
  }
  oos::ui::StatusBarAppearance statusBarAppearance() const override {
    return appearance;
  }
  void setStatusBarVisible(bool value) override { visible = value; }

  oos::ui::StatusBarAppearance appearance;
  int apply_count = 0;
  int set_count = 0;
  bool visible = true;
};

class FakeApplicationSession final : public oos::runtime::ApplicationSession {
public:
  FakeApplicationSession(oos::ui::StatusBarAppearanceController &status_bar,
                         oos::ui::StatusBarAppearance appearance)
      : status_bar_(status_bar), appearance_(appearance) {}

  bool initialize() override {
    initialized = true;
    status_bar_.setStatusBarAppearance(appearance_);
    return true;
  }
  void shutdown() override { initialized = false; }
  bool frame(int64_t, uint32_t &next_delay_ms) override {
    next_delay_ms = 1000;
    return initialized;
  }
  std::string takeLaunchRequest() override { return {}; }
  bool takeExitRequest() override { return false; }
  bool dispatchKey(const oos::input::KeyEvent &, int64_t) override {
    ++key_count;
    return initialized;
  }
  const char *lastError() const override { return ""; }

  int key_count = 0;
  bool initialized = false;

private:
  oos::ui::StatusBarAppearanceController &status_bar_;
  oos::ui::StatusBarAppearance appearance_;
};

lv_obj_t *findScrollableList(lv_obj_t *object) {
  if (!object || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN))
    return nullptr;
  if (lv_obj_get_height(object) == 238 &&
      lv_obj_has_flag(object, LV_OBJ_FLAG_SCROLLABLE))
    return object;
  const uint32_t children = lv_obj_get_child_count(object);
  for (uint32_t index = 0; index < children; ++index) {
    if (lv_obj_t *match = findScrollableList(lv_obj_get_child(object, index)))
      return match;
  }
  return nullptr;
}

void assertLabelsWithinParents(lv_obj_t *object) {
  if (!object || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN))
    return;
  if (lv_obj_check_type(object, &lv_label_class)) {
    lv_obj_t *parent = lv_obj_get_parent(object);
    assert(parent);
    lv_area_t label_area = {};
    lv_area_t parent_area = {};
    lv_obj_get_coords(object, &label_area);
    lv_obj_get_coords(parent, &parent_area);
    if (label_area.x1 < parent_area.x1 || label_area.x2 > parent_area.x2 ||
        label_area.y1 < parent_area.y1 || label_area.y2 > parent_area.y2) {
      std::fprintf(stderr,
                   "label overflow: '%s' label=(%d,%d)-(%d,%d) "
                   "parent=(%d,%d)-(%d,%d)\n",
                   lv_label_get_text(object), label_area.x1, label_area.y1,
                   label_area.x2, label_area.y2, parent_area.x1, parent_area.y1,
                   parent_area.x2, parent_area.y2);
    }
    assert(label_area.x1 >= parent_area.x1);
    assert(label_area.x2 <= parent_area.x2);
    assert(label_area.y1 >= parent_area.y1);
    assert(label_area.y2 <= parent_area.y2);
  }
  const uint32_t children = lv_obj_get_child_count(object);
  for (uint32_t index = 0; index < children; ++index)
    assertLabelsWithinParents(lv_obj_get_child(object, index));
}

void testLvglBackend() {
  FakeGraphicsHost graphics;
  oos::sdk::ui::LvglBackend backend(graphics);
  assert(backend.initialize());
  lv_font_glyph_dsc_t chinese = {};
  assert(
      lv_font_get_glyph_dsc(oos::sdk::ui::fonts::get(12), &chinese, 0x4e2d, 0));
  assert(chinese.resolved_font != nullptr);
  lv_obj_t *label = lv_label_create(backend.root());
  lv_obj_set_style_text_font(label, oos::sdk::ui::fonts::get(12), 0);
  lv_label_set_text(label, "LVGL backend 中文");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
  backend.frame(1000);
  backend.frame(20000);
  backend.frame(40000);
  assert(backend.healthy());
  assert(graphics.texture_uploads > 1);
  assert(graphics.frame_submissions > 0);
  backend.shutdown();
  assert(graphics.textures.empty());
}

void testApplicationSessions() {
  FakeGraphicsHost graphics;
  oos::compositor::Compositor compositor(graphics);
  FakeStatusBarAppearance status_bar;
  oos::runtime::ApplicationSessionManager sessions(
      compositor, 0, 22, 240, 298, status_bar, {0x010203, false});
  FakeApplicationSession *first = nullptr;
  FakeApplicationSession *second = nullptr;
  oos::runtime::GraphicsHost *first_surface = nullptr;
  oos::ui::StatusBarAppearanceController *first_appearance = nullptr;
  assert(sessions.registerFactory(
      "cc.jaxy.test.first",
      [&](oos::runtime::GraphicsHost &surface,
          oos::ui::StatusBarAppearanceController &appearance) {
        auto session = std::make_unique<FakeApplicationSession>(
            appearance, oos::ui::StatusBarAppearance{0x112233, false});
        first = session.get();
        first_surface = &surface;
        first_appearance = &appearance;
        return session;
      }));
  assert(sessions.registerFactory(
      "cc.jaxy.test.second",
      [&](oos::runtime::GraphicsHost &,
          oos::ui::StatusBarAppearanceController &appearance) {
        auto session = std::make_unique<FakeApplicationSession>(
            appearance, oos::ui::StatusBarAppearance{0xaabbcc, true});
        second = session.get();
        return session;
      }));
  assert(sessions.activate("cc.jaxy.test.first"));
  assert(first && sessions.residentCount() == 1);
  assert(first_surface->height() == 298);
  assert(first_appearance->setSurfaceMode(oos::ui::SurfaceMode::Immersive));
  assert(first_surface->height() == 320 && !status_bar.visible);
  assert(first_appearance->setSurfaceMode(oos::ui::SurfaceMode::Normal));
  assert(first_surface->height() == 298 && status_bar.visible);
  assert(status_bar.appearance ==
         (oos::ui::StatusBarAppearance{0x112233, false}));
  const oos::input::KeyEvent key = {
      1000, 352, oos::input::KeyAction::Pressed, {}, {}};
  assert(sessions.dispatchKey(key, 1000));
  assert(first->key_count == 1);

  // Registration may grow the catalog, but resident session addresses remain
  // stable and activation never reconstructs an existing application.
  assert(sessions.registerFactory(
      "cc.jaxy.test.third",
      [](oos::runtime::GraphicsHost &,
         oos::ui::StatusBarAppearanceController &appearance) {
        return std::make_unique<FakeApplicationSession>(
            appearance, oos::ui::StatusBarAppearance{0x445566, false});
      }));
  assert(sessions.activate("cc.jaxy.test.second"));
  assert(second && sessions.residentCount() == 2);
  assert(status_bar.appearance ==
         (oos::ui::StatusBarAppearance{0xaabbcc, true}));
  assert(sessions.dispatchKey(key, 1100));
  assert(second->key_count == 1);
  assert(sessions.activate("cc.jaxy.test.first"));
  assert(status_bar.appearance ==
         (oos::ui::StatusBarAppearance{0x112233, false}));
  assert(sessions.dispatchKey(key, 1200));
  assert(first->key_count == 2);
  assert(std::string(sessions.activeId()) == "cc.jaxy.test.first");
  assert(sessions.activate("cc.jaxy.test.second"));
  assert(sessions.destroy("cc.jaxy.test.first"));
  assert(sessions.residentCount() == 1);
  assert(sessions.activate("cc.jaxy.test.first"));
  assert(first && first->key_count == 0);
  sessions.shutdown();
  assert(sessions.residentCount() == 0);
}

void testTransparentLvglBackend() {
  FakeGraphicsHost graphics;
  oos::sdk::ui::LvglBackend backend(graphics,
                                    oos::sdk::ui::LvglBackendOptions{true});
  assert(backend.initialize());
  lv_obj_t *panel = lv_obj_create(backend.root());
  lv_obj_set_size(panel, 100, 40);
  lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 8);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x202326), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  backend.frame(1000);
  assert(backend.refresh());
  assert(graphics.texture_uploads > 0);
  assert(graphics.frame_submissions > 0);
  assert(graphics.draw_commands > 0);
  backend.shutdown();
  assert(graphics.textures.empty());
}

void testSystemUi() {
  FakeGraphicsHost graphics;
  oos::compositor::Compositor compositor(graphics);
  auto status = compositor.createLayer({"status", 0, 0, 240, 22, 200});
  auto overlay = compositor.createLayer({"overlay", 0, 22, 240, 298, 100});
  assert(status && overlay);
  FakeStatusSource statuses;
  statuses.value.revision = 1;
  statuses.value.battery_available = true;
  statuses.value.battery_percent = 82;
  statuses.value.charging = true;
  statuses.value.wifi_available = true;
  statuses.value.wifi_connected = true;
  statuses.value.cellular_available = true;
  statuses.value.cellular_registered = true;
  statuses.value.signal_bars = 3;
  statuses.value.radio_technology = "4G";
  oos::apps::systemui::SystemUi system_ui(*status, *overlay, &statuses);
  assert(system_ui.initialize());
  system_ui.applyStatusBarAppearance({0xf2f2f2, true});
  uint32_t next_delay_ms = 0;
  assert(system_ui.frame(1000, next_delay_ms));
  assert(compositor.compose());
  assert(next_delay_ms > 0 && next_delay_ms <= 1000);
  const size_t home_frames = graphics.frame_submissions;

  statuses.value.revision = 2;
  statuses.value.battery_percent = 64;
  assert(system_ui.frame(1500, next_delay_ms));
  assert(compositor.compose());
  assert(graphics.frame_submissions > home_frames);

  system_ui.showNotification("Incoming message", 2000, 3000);
  assert(system_ui.frame(2000, next_delay_ms));
  assert(compositor.compose());
  assert(overlay->visible());

  system_ui.setLocked(true);
  assert(system_ui.frame(2500, next_delay_ms));
  assert(compositor.compose());
  bool consumed = false;
  const oos::input::KeyEvent unlock = {
      3000, 352, oos::input::KeyAction::Pressed, {}, {}};
  assert(system_ui.routeKey(unlock, consumed));
  assert(consumed && !system_ui.locked());
  system_ui.shutdown();
  assert(graphics.textures.empty());
}

void testLauncher() {
  FakeGraphicsHost graphics;
  oos::compositor::Compositor compositor(graphics);
  auto app = compositor.createLayer({"application", 0, 22, 240, 298, 0});
  assert(app);
  const std::string data_root =
      "/tmp/oos-launcher-test-" + std::to_string(getpid());
  std::filesystem::remove_all(data_root);
  oos::apps::AppRepository repository(data_root.c_str());
  assert(repository.initialize());
  FakeStatusBarAppearance status_bar;
  oos::apps::launcher::Launcher launcher(*app, repository, status_bar);
  assert(launcher.initialize());
  assert(status_bar.appearance ==
         (oos::ui::StatusBarAppearance{oos::sdk::ui::theme::kCanvas, false}));
  uint32_t next_delay_ms = 0;
  assert(launcher.frame(1000, next_delay_ms));
  assert(compositor.compose());
  const size_t home_frames = graphics.frame_submissions;

  const oos::input::KeyEvent open_apps = {
      2000, 352, oos::input::KeyAction::Pressed, {}, {}};
  assert(launcher.dispatchKey(open_apps, 2000));
  assert(launcher.frame(2000, next_delay_ms));
  assert(compositor.compose());
  assert(graphics.frame_submissions > home_frames);
  assertLabelsWithinParents(
      lv_display_get_screen_active(lv_display_get_default()));

  const oos::input::KeyEvent select_settings = {
      2100, 105, oos::input::KeyAction::Pressed, {}, {}};
  assert(launcher.dispatchKey(select_settings, 2100));
  assert(launcher.dispatchKey(open_apps, 2200));
  assert(launcher.takeLaunchRequest() == "cc.jaxy.oos.settings");

  launcher.shutdown();
  assert(graphics.textures.empty());
  std::filesystem::remove_all(data_root);
}

void testSettings() {
  FakeGraphicsHost graphics;
  oos::compositor::Compositor compositor(graphics);
  auto launcher_app = compositor.createLayer({"launcher", 0, 22, 240, 298, 0});
  auto app = compositor.createLayer({"settings", 0, 22, 240, 298, 0});
  auto overlay = compositor.createLayer({"overlay", 0, 22, 240, 298, 100});
  auto status = compositor.createLayer({"status", 0, 0, 240, 22, 200});
  assert(launcher_app && app && overlay && status);
  const std::string data_root =
      "/tmp/oos-settings-test-" + std::to_string(getpid());
  std::filesystem::remove_all(data_root);
  oos::apps::AppRepository repository(data_root.c_str());
  assert(repository.initialize());
  oos::ui::SystemUiSettings preferences(data_root);
  assert(preferences.initialize());
  FakeStatusSource statuses;
  statuses.value.revision = 1;
  statuses.value.battery_available = true;
  statuses.value.battery_percent = 82;
  oos::apps::systemui::SystemUi system_ui(*status, *overlay, &statuses,
                                          &preferences);
  assert(system_ui.initialize());
  std::unique_ptr<oos::device::Device> device = oos::device::createDevice();
  assert(device);
  {
    oos::device::ServiceProvider wifi(*device);
    bool enabled = false;
    assert(wifi.wifiEnabled(enabled) && enabled);
    std::vector<oos::network::WifiAccessPoint> access_points;
    assert(wifi.wifiScan(access_points, 0) && access_points.size() == 3);
    assert(wifi.wifiSetEnabled(false));
    assert(wifi.wifiEnabled(enabled) && !enabled);
    assert(wifi.wifiSetEnabled(true));
    int network_id = -1;
    assert(wifi.wifiConnect("Orange Lab", oos::network::WifiSecurity::WpaPsk,
                            "orangeos", network_id));
    assert(network_id >= 0);
    oos::network::WifiStatus wifi_status;
    assert(wifi.wifiStatus(wifi_status) && wifi_status.ssid == "Orange Lab");
    assert(wifi.wifiDisconnect());
    assert(wifi.wifiStatus(wifi_status) && wifi_status.state == "DISCONNECTED");
    assert(wifi.wifiSelect(1));
    assert(wifi.wifiForget(1));
    std::vector<oos::network::WifiNetwork> saved;
    assert(wifi.wifiListNetworks(saved) && saved.size() == 1);
  }
  FakeStatusBarAppearance launcher_status_bar;
  FakeStatusBarAppearance settings_status_bar;
  oos::apps::launcher::Launcher launcher(*launcher_app, repository,
                                         launcher_status_bar);
  assert(launcher.initialize());
  const oos::input::KeyEvent open_apps = {
      900, 352, oos::input::KeyAction::Pressed, {}, {}};
  const oos::input::KeyEvent select_settings = {
      901, 105, oos::input::KeyAction::Pressed, {}, {}};
  assert(launcher.dispatchKey(open_apps, 900));
  assert(launcher.dispatchKey(select_settings, 901));
  assert(launcher.dispatchKey(open_apps, 902));
  assert(launcher.takeLaunchRequest() == "cc.jaxy.oos.settings");
  launcher_app->setVisible(false);

  oos::apps::settings::Settings settings(*app, repository, *device, preferences,
                                         settings_status_bar, data_root);
  assert(settings.initialize());
  assert(settings_status_bar.appearance ==
         (oos::ui::StatusBarAppearance{oos::sdk::ui::theme::kSurface, false}));
  const oos::input::KeyEvent return_to_launcher = {
      950, 357, oos::input::KeyAction::Pressed, {}, {}};
  assert(settings.dispatchKey(return_to_launcher, 950));
  assert(settings.takeLaunchRequest() == "cc.jaxy.oos.launcher");
  app->setVisible(false);
  launcher_app->setVisible(true);
  assert(launcher.dispatchKey(open_apps, 951));
  assert(launcher.takeLaunchRequest() == "cc.jaxy.oos.settings");
  launcher_app->setVisible(false);
  app->setVisible(true);

  uint32_t next_delay_ms = 0;
  assert(system_ui.frame(1000, next_delay_ms));
  assert(settings.frame(1000, next_delay_ms));
  assert(compositor.compose());
  for (lv_display_t *display = lv_display_get_next(nullptr); display;
       display = lv_display_get_next(display))
    assertLabelsWithinParents(lv_display_get_screen_active(display));

  const oos::input::KeyEvent wifi_ok = {
      1100, 352, oos::input::KeyAction::Pressed, {}, {}};
  const oos::input::KeyEvent wifi_down = {
      52000, 108, oos::input::KeyAction::Pressed, {}, {}};
  assert(settings.dispatchKey(wifi_ok, 1100));
  for (int count = 0; count < 50; ++count) {
    usleep(1000);
    assert(settings.frame(1200 + count * 1000, next_delay_ms));
  }
  assert(settings.dispatchKey(wifi_down, 52000));
  assert(settings.dispatchKey(wifi_down, 53000));
  assert(settings.dispatchKey(wifi_ok, 54000));
  for (int count = 0; count < 50; ++count) {
    usleep(1000);
    assert(settings.frame(55000 + count * 1000, next_delay_ms));
  }
  for (lv_display_t *display = lv_display_get_next(nullptr); display;
       display = lv_display_get_next(display))
    assertLabelsWithinParents(lv_display_get_screen_active(display));

  const oos::input::KeyEvent wifi_back = {
      106000, 357, oos::input::KeyAction::Pressed, {}, {}};
  assert(settings.dispatchKey(wifi_back, 106000));
  assert(settings.dispatchKey(wifi_back, 107000));

  lv_obj_t *settings_list = nullptr;
  for (lv_display_t *display = lv_display_get_next(nullptr); display;
       display = lv_display_get_next(display)) {
    settings_list = findScrollableList(lv_display_get_screen_active(display));
    if (settings_list)
      break;
  }
  assert(settings_list);
  assert(lv_obj_get_scroll_bottom(settings_list) > 0);
  assert(lv_obj_get_scroll_y(settings_list) == 0);

  const oos::input::KeyEvent down = {
      2000, 108, oos::input::KeyAction::Pressed, {}, {}};
  const oos::input::KeyEvent ok = {
      2000, 352, oos::input::KeyAction::Pressed, {}, {}};
  for (int count = 0; count < 6; ++count)
    assert(settings.dispatchKey(down, 2000 + count));
  const int retained_scroll_y = lv_obj_get_scroll_y(settings_list);
  assert(retained_scroll_y > 0);
  assert(settings.dispatchKey(ok, 2100));
  assert(settings.dispatchKey(ok, 2200));
  assert(!preferences.statusBar().show_clock);

  const oos::input::KeyEvent back = {
      2250, 357, oos::input::KeyAction::Pressed, {}, {}};
  assert(settings.dispatchKey(back, 2250));
  lv_obj_t *restored_list = nullptr;
  for (lv_display_t *display = lv_display_get_next(nullptr); display;
       display = lv_display_get_next(display)) {
    restored_list = findScrollableList(lv_display_get_screen_active(display));
    if (restored_list)
      break;
  }
  assert(restored_list == settings_list);
  assert(lv_obj_get_scroll_y(restored_list) == retained_scroll_y);

  assert(system_ui.frame(2300, next_delay_ms));
  assert(settings.frame(2300, next_delay_ms));
  assert(compositor.compose());

  oos::ui::SystemUiSettings reloaded(data_root);
  assert(reloaded.initialize());
  assert(!reloaded.statusBar().show_clock);
  settings.shutdown();
  launcher.shutdown();
  system_ui.shutdown();
  assert(graphics.textures.empty());
  std::filesystem::remove_all(data_root);
}

} // namespace

int main() {
  testLvglBackend();
  testApplicationSessions();
  testTransparentLvglBackend();
  testSystemUi();
  testLauncher();
  testSettings();
  return 0;
}
