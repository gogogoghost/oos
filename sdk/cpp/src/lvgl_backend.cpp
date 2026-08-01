#include "oos/sdk/ui/lvgl_backend.h"

#include "oos/runtime/graphics_host.h"

#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <vector>

namespace oos::sdk::ui {
namespace {

constexpr uint32_t kTextureHandle = 0x7fff0001u;
constexpr uint32_t kDrawRows = 32;
constexpr uint32_t kMaximumTickAdvanceMs = 250;
size_t g_lvgl_users = 0;
int64_t g_last_tick_us = 0;

uint32_t mapKey(uint16_t code) {
  switch (code) {
  case 2:
    return '1';
  case 3:
    return '2';
  case 4:
    return '3';
  case 5:
    return '4';
  case 6:
    return '5';
  case 7:
    return '6';
  case 8:
    return '7';
  case 9:
    return '8';
  case 10:
    return '9';
  case 11:
    return '0';
  case 103:
    return LV_KEY_UP;
  case 105:
    return LV_KEY_LEFT;
  case 106:
    return LV_KEY_RIGHT;
  case 108:
    return LV_KEY_DOWN;
  case 139:
    return LV_KEY_PREV;
  case 158:
    return LV_KEY_ESC;
  case 352:
    return LV_KEY_ENTER;
  case 357:
    return LV_KEY_NEXT;
  default:
    return 0;
  }
}

} // namespace

class LvglBackend::Impl {
public:
  struct QueuedKey {
    uint32_t key = 0;
    lv_indev_state_t state = LV_INDEV_STATE_RELEASED;
  };

  explicit Impl(runtime::GraphicsHost &graphics) : graphics(graphics) {}

  static void flush(lv_display_t *display, const lv_area_t *area,
                    uint8_t *pixels) {
    auto *self = static_cast<Impl *>(lv_display_get_user_data(display));
    if (!self || !area || !pixels)
      return;
    const uint32_t width = static_cast<uint32_t>(lv_area_get_width(area));
    const uint32_t height = static_cast<uint32_t>(lv_area_get_height(area));
    const uint32_t stride = width * sizeof(uint16_t);
    const size_t bytes = static_cast<size_t>(stride) * height;
    if (!self->graphics.setTexture(kTextureHandle, OOS_TEXTURE_RGB565,
                                   static_cast<uint32_t>(area->x1),
                                   static_cast<uint32_t>(area->y1), width,
                                   height, stride, 0, pixels, bytes)) {
      self->error = "LVGL dirty texture upload failed";
      self->healthy = false;
    }
    if (lv_display_flush_is_last(display) && self->healthy &&
        !self->present()) {
      self->error = "LVGL frame submission failed";
      self->healthy = false;
    }
    lv_display_flush_ready(display);
  }

  static void readKey(lv_indev_t *input, lv_indev_data_t *data) {
    auto *self = static_cast<Impl *>(lv_indev_get_user_data(input));
    if (!self || self->keys.empty()) {
      data->continue_reading = false;
      return;
    }
    const QueuedKey key = self->keys.front();
    self->keys.pop_front();
    data->key = key.key;
    data->state = key.state;
    data->continue_reading = !self->keys.empty();
  }

  bool initialize() {
    if (initialized)
      return true;
    if (graphics.width() == 0 || graphics.height() == 0 ||
        graphics.width() > std::numeric_limits<uint16_t>::max() ||
        graphics.height() > std::numeric_limits<uint16_t>::max()) {
      error = "LVGL surface dimensions are invalid";
      return false;
    }
    if (g_lvgl_users == 0)
      lv_init();
    ++g_lvgl_users;
    lv_started = true;

    const size_t row_pixels = static_cast<size_t>(graphics.width()) * kDrawRows;
    draw_buffer_a.resize(row_pixels);
    draw_buffer_b.resize(row_pixels);
    display = lv_display_create(static_cast<int32_t>(graphics.width()),
                                static_cast<int32_t>(graphics.height()));
    if (!display) {
      error = "LVGL display allocation failed";
      shutdown();
      return false;
    }
    lv_display_set_user_data(display, this);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer_a.data(), draw_buffer_b.data(),
                           row_pixels * sizeof(uint16_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);

    group = lv_group_create();
    input = lv_indev_create();
    if (!group || !input) {
      error = "LVGL input allocation failed";
      shutdown();
      return false;
    }
    lv_group_set_default(group);
    lv_indev_set_type(input, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_group(input, group);
    lv_indev_set_user_data(input, this);
    lv_indev_set_read_cb(input, readKey);

    std::vector<uint16_t> blank(
        static_cast<size_t>(graphics.width()) * graphics.height(), 0);
    if (!graphics.setTexture(
            kTextureHandle, OOS_TEXTURE_RGB565, 0, 0, graphics.width(),
            graphics.height(), graphics.width() * sizeof(uint16_t),
            OOS_TEXTURE_REPLACE | OOS_TEXTURE_LINEAR_MINIFICATION |
                OOS_TEXTURE_LINEAR_MAGNIFICATION,
            reinterpret_cast<const uint8_t *>(blank.data()),
            blank.size() * sizeof(uint16_t))) {
      error = "LVGL surface texture allocation failed";
      shutdown();
      return false;
    }
    texture_live = true;
    healthy = true;
    initialized = true;
    error.clear();
    return true;
  }

  void shutdown() {
    keys.clear();
    if (texture_live) {
      graphics.freeTexture(kTextureHandle);
      texture_live = false;
    }
    if (input)
      lv_indev_delete(input);
    if (group)
      lv_group_delete(group);
    if (display)
      lv_display_delete(display);
    display = nullptr;
    input = nullptr;
    group = nullptr;
    if (lv_started) {
      if (g_lvgl_users > 0)
        --g_lvgl_users;
      if (g_lvgl_users == 0)
        lv_deinit();
      if (g_lvgl_users == 0)
        g_last_tick_us = 0;
      lv_started = false;
    }
    draw_buffer_a.clear();
    draw_buffer_b.clear();
    initialized = false;
    healthy = false;
  }

  bool present() {
    const float width = static_cast<float>(graphics.width());
    const float height = static_cast<float>(graphics.height());
    const std::array<OosGfxVertex, 4> vertices = {{
        {{0, 0}, {0, 0}, {255, 255, 255, 255}},
        {{width, 0}, {1, 0}, {255, 255, 255, 255}},
        {{0, height}, {0, 1}, {255, 255, 255, 255}},
        {{width, height}, {1, 1}, {255, 255, 255, 255}},
    }};
    constexpr std::array<uint16_t, 6> indices = {0, 1, 2, 2, 1, 3};
    const OosGfxDrawCommand command = {
        0, 6, kTextureHandle, {0, 0}, {width, height}};
    return graphics.submit(vertices.data(), vertices.size(), indices.data(),
                           indices.size(), &command, 1, 0xff000000u);
  }

  runtime::GraphicsHost &graphics;
  lv_display_t *display = nullptr;
  lv_indev_t *input = nullptr;
  lv_group_t *group = nullptr;
  std::vector<uint16_t> draw_buffer_a;
  std::vector<uint16_t> draw_buffer_b;
  std::deque<QueuedKey> keys;
  std::string error;
  bool lv_started = false;
  bool texture_live = false;
  bool initialized = false;
  bool healthy = false;
};

LvglBackend::LvglBackend(runtime::GraphicsHost &graphics)
    : impl_(std::make_unique<Impl>(graphics)) {}

LvglBackend::~LvglBackend() { shutdown(); }

bool LvglBackend::initialize() { return impl_->initialize(); }

void LvglBackend::shutdown() {
  if (impl_)
    impl_->shutdown();
}

bool LvglBackend::dispatchKey(const input::KeyEvent &event) {
  if (!impl_->initialized)
    return false;
  const uint32_t key = mapKey(event.code);
  if (key == 0)
    return true;
  impl_->keys.push_back({key, event.action == input::KeyAction::Released
                                  ? LV_INDEV_STATE_RELEASED
                                  : LV_INDEV_STATE_PRESSED});
  return true;
}

uint32_t LvglBackend::frame(int64_t monotonic_us) {
  if (!impl_->initialized || !impl_->healthy)
    return 0;
  if (g_last_tick_us != 0 && monotonic_us > g_last_tick_us) {
    const uint64_t elapsed =
        static_cast<uint64_t>(monotonic_us - g_last_tick_us) / 1000;
    lv_tick_inc(static_cast<uint32_t>(
        std::min<uint64_t>(elapsed, kMaximumTickAdvanceMs)));
  }
  if (monotonic_us > g_last_tick_us)
    g_last_tick_us = monotonic_us;
  const uint32_t next = lv_timer_handler();
  return impl_->healthy ? next : 0;
}

bool LvglBackend::refresh() {
  if (!impl_->initialized || !impl_->healthy)
    return false;
  lv_refr_now(impl_->display);
  return impl_->healthy;
}

bool LvglBackend::healthy() const {
  return impl_->initialized && impl_->healthy;
}

lv_obj_t *LvglBackend::root() const {
  return impl_->initialized ? lv_display_get_screen_active(impl_->display)
                            : nullptr;
}

lv_group_t *LvglBackend::group() const { return impl_->group; }

const std::string &LvglBackend::lastError() const { return impl_->error; }

} // namespace oos::sdk::ui
