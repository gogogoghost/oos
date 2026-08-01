#include "oos/apps/app_repository.h"
#include "oos/runtime/graphics_host.h"
#include "oos/ui/imgui_backend.h"
#include "oos/ui/lvgl_backend.h"
#include "oos/ui/system_ui.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <unordered_set>

namespace {

class FakeGraphicsHost final : public oos::runtime::GraphicsHost {
public:
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

void testLvglBackend() {
  FakeGraphicsHost graphics;
  oos::ui::LvglBackend backend(graphics);
  assert(backend.initialize());
  lv_obj_t *label = lv_label_create(backend.root());
  lv_label_set_text(label, "LVGL backend");
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

void testImguiBackend() {
  FakeGraphicsHost graphics;
  oos::ui::ImguiBackend backend(graphics);
  assert(backend.initialize());
  assert(backend.beginFrame(1000));
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImVec2(240, 320));
  ImGui::Begin("Backend test", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings);
  ImGui::TextUnformatted("Dear ImGui backend");
  ImGui::Button("OK");
  ImGui::End();
  assert(backend.submit());
  assert(graphics.texture_uploads > 0);
  assert(graphics.frame_submissions == 1);
  assert(graphics.draw_commands > 0);
  backend.shutdown();
  assert(graphics.textures.empty());
}

void testSystemUi() {
  char path[] = "/tmp/oos-system-ui.XXXXXX";
  assert(mkdtemp(path));
  oos::apps::AppRepository repository(path);
  assert(repository.initialize());
  FakeGraphicsHost graphics;
  oos::ui::SystemUi system_ui(graphics, repository);
  assert(system_ui.initialize());
  uint32_t next_delay_ms = 0;
  assert(system_ui.frame(1000, next_delay_ms));
  assert(next_delay_ms > 0);
  const size_t home_frames = graphics.frame_submissions;
  const oos::input::KeyEvent open_apps = {
      2000, 352, oos::input::KeyAction::Pressed, {}, {}};
  assert(system_ui.dispatchKey(open_apps));
  assert(system_ui.frame(2000, next_delay_ms));
  assert(graphics.frame_submissions > home_frames);
  system_ui.shutdown();
  assert(graphics.textures.empty());
  std::filesystem::remove_all(path);
}

} // namespace

int main() {
  testLvglBackend();
  testImguiBackend();
  testSystemUi();
  return 0;
}
