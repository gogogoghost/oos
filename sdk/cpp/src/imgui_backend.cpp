#include "oos/sdk/ui/imgui_backend.h"

#include "oos/runtime/graphics_host.h"
#include "oos/sdk/ui/fonts.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace oos::sdk::ui {
namespace {

constexpr uint32_t kFirstTextureHandle = 0x7ffe0001u;

ImGuiKey mapKey(uint16_t code) {
  switch (code) {
  case 2:
    return ImGuiKey_1;
  case 3:
    return ImGuiKey_2;
  case 4:
    return ImGuiKey_3;
  case 5:
    return ImGuiKey_4;
  case 6:
    return ImGuiKey_5;
  case 7:
    return ImGuiKey_6;
  case 8:
    return ImGuiKey_7;
  case 9:
    return ImGuiKey_8;
  case 10:
    return ImGuiKey_9;
  case 11:
    return ImGuiKey_0;
  case 103:
    return ImGuiKey_UpArrow;
  case 105:
    return ImGuiKey_LeftArrow;
  case 106:
    return ImGuiKey_RightArrow;
  case 108:
    return ImGuiKey_DownArrow;
  case 139:
    return ImGuiKey_F1;
  case 158:
    return ImGuiKey_Escape;
  case 352:
    return ImGuiKey_Enter;
  case 357:
    return ImGuiKey_F2;
  default:
    return ImGuiKey_None;
  }
}

uint8_t colorChannel(ImU32 color, int shift) {
  return static_cast<uint8_t>((color >> shift) & 0xffu);
}

uint8_t premultiply(uint8_t channel, uint8_t alpha) {
  return static_cast<uint8_t>((static_cast<uint32_t>(channel) * alpha + 127u) /
                              255u);
}

void resetRenderState(const ImDrawList *, const ImDrawCmd *) {}

} // namespace

class ImguiBackend::Impl {
public:
  explicit Impl(runtime::GraphicsHost &graphics) : graphics(graphics) {}

  bool initialize() {
    if (initialized)
      return true;
    if (graphics.width() == 0 || graphics.height() == 0) {
      error = "Dear ImGui surface dimensions are invalid";
      return false;
    }
    IMGUI_CHECKVERSION();
    context = ImGui::CreateContext();
    if (!context) {
      error = "Dear ImGui context allocation failed";
      return false;
    }
    ImGui::SetCurrentContext(context);
    ImGuiIO &io = ImGui::GetIO();
    const std::string system_font = fonts::regularPath();
    if (system_font.empty() ||
        !io.Fonts->AddFontFromFileTTF(system_font.c_str(), 14.0f)) {
      error = system_font.empty() ? "no readable system UI font was found"
                                  : "load ImGui system font failed";
      ImGui::DestroyContext(context);
      context = nullptr;
      return false;
    }
    io.BackendPlatformName = "oos_input";
    io.BackendRendererName = "oos_graphics_host";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset |
                       ImGuiBackendFlags_RendererHasTextures |
                       ImGuiBackendFlags_HasGamepad;
    io.ConfigFlags |=
        ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.DisplaySize = ImVec2(static_cast<float>(graphics.width()),
                            static_cast<float>(graphics.height()));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.IniFilename = nullptr;
    ImGui::GetPlatformIO().DrawCallback_ResetRenderState = resetRenderState;
    ImGui::StyleColorsDark();
    initialized = true;
    error.clear();
    return true;
  }

  void shutdown() {
    if (!context)
      return;
    ImGui::SetCurrentContext(context);
    for (ImTextureData *texture : ImGui::GetPlatformIO().Textures) {
      if (texture->TexID != ImTextureID_Invalid) {
        graphics.freeTexture(static_cast<uint32_t>(texture->TexID));
        texture->SetTexID(ImTextureID_Invalid);
        texture->SetStatus(ImTextureStatus_Destroyed);
      }
    }
    ImGuiIO &io = ImGui::GetIO();
    io.BackendPlatformName = nullptr;
    io.BackendRendererName = nullptr;
    io.BackendFlags &=
        ~(ImGuiBackendFlags_RendererHasVtxOffset |
          ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_HasGamepad);
    ImGui::GetPlatformIO().ClearRendererHandlers();
    ImGui::DestroyContext(context);
    context = nullptr;
    vertices.clear();
    indices.clear();
    commands.clear();
    texture_scratch.clear();
    initialized = false;
    frame_started = false;
    last_frame_us = 0;
    next_texture = kFirstTextureHandle;
  }

  bool dispatchKey(const input::KeyEvent &event) {
    if (!initialized)
      return false;
    ImGui::SetCurrentContext(context);
    const ImGuiKey key = mapKey(event.code);
    if (key == ImGuiKey_None)
      return true;
    const bool down = event.action != input::KeyAction::Released;
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(key, down);
    switch (key) {
    case ImGuiKey_UpArrow:
      io.AddKeyEvent(ImGuiKey_GamepadDpadUp, down);
      break;
    case ImGuiKey_DownArrow:
      io.AddKeyEvent(ImGuiKey_GamepadDpadDown, down);
      break;
    case ImGuiKey_LeftArrow:
      io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, down);
      break;
    case ImGuiKey_RightArrow:
      io.AddKeyEvent(ImGuiKey_GamepadDpadRight, down);
      break;
    case ImGuiKey_Enter:
      io.AddKeyEvent(ImGuiKey_GamepadFaceDown, down);
      break;
    case ImGuiKey_Escape:
      io.AddKeyEvent(ImGuiKey_GamepadFaceRight, down);
      break;
    default:
      break;
    }
    return true;
  }

  bool beginFrame(int64_t monotonic_us) {
    if (!initialized || frame_started)
      return false;
    ImGui::SetCurrentContext(context);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(graphics.width()),
                            static_cast<float>(graphics.height()));
    if (last_frame_us != 0 && monotonic_us > last_frame_us) {
      const float elapsed =
          static_cast<float>(monotonic_us - last_frame_us) / 1000000.0f;
      io.DeltaTime = std::clamp(elapsed, 1.0f / 1000.0f, 0.25f);
    } else {
      io.DeltaTime = 1.0f / 60.0f;
    }
    last_frame_us = monotonic_us;
    ImGui::NewFrame();
    frame_started = true;
    return true;
  }

  bool updateTexture(ImTextureData &texture) {
    if (texture.Status == ImTextureStatus_WantCreate) {
      if (texture.Width <= 0 || texture.Height <= 0 || !texture.Pixels ||
          next_texture == 0) {
        error = "Dear ImGui requested an invalid texture";
        return false;
      }
      const uint32_t handle = next_texture++;
      if (!uploadTexture(handle, texture, 0, 0,
                         static_cast<uint32_t>(texture.Width),
                         static_cast<uint32_t>(texture.Height),
                         OOS_TEXTURE_REPLACE | OOS_TEXTURE_LINEAR_MINIFICATION |
                             OOS_TEXTURE_LINEAR_MAGNIFICATION)) {
        error = "Dear ImGui texture creation failed";
        return false;
      }
      texture.SetTexID(static_cast<ImTextureID>(handle));
      texture.SetStatus(ImTextureStatus_OK);
      return true;
    }
    if (texture.Status == ImTextureStatus_WantUpdates) {
      if (texture.TexID == ImTextureID_Invalid || !texture.Pixels) {
        error = "Dear ImGui requested an update for an invalid texture";
        return false;
      }
      const uint32_t handle = static_cast<uint32_t>(texture.TexID);
      for (const ImTextureRect &rect : texture.Updates) {
        if (rect.w == 0 || rect.h == 0)
          continue;
        if (!uploadTexture(handle, texture, rect.x, rect.y, rect.w, rect.h,
                           0)) {
          error = "Dear ImGui texture update failed";
          return false;
        }
      }
      texture.SetStatus(ImTextureStatus_OK);
      return true;
    }
    if (texture.Status == ImTextureStatus_WantDestroy) {
      if (texture.UnusedFrames <= 0)
        return true;
      if (texture.TexID != ImTextureID_Invalid &&
          !graphics.freeTexture(static_cast<uint32_t>(texture.TexID))) {
        error = "Dear ImGui texture destruction failed";
        return false;
      }
      texture.SetTexID(ImTextureID_Invalid);
      texture.SetStatus(ImTextureStatus_Destroyed);
    }
    return true;
  }

  bool uploadTexture(uint32_t handle, ImTextureData &texture, uint32_t x,
                     uint32_t y, uint32_t width, uint32_t height,
                     uint32_t flags) {
    if (texture.Format == ImTextureFormat_Alpha8) {
      const size_t bytes = static_cast<size_t>(texture.GetPitch()) *
                               (static_cast<size_t>(height) - 1) +
                           width;
      return graphics.setTexture(
          handle, OOS_TEXTURE_A8, x, y, width, height,
          static_cast<uint32_t>(texture.GetPitch()), flags,
          static_cast<const uint8_t *>(texture.GetPixelsAt(x, y)), bytes);
    }
    if (texture.Format != ImTextureFormat_RGBA32)
      return false;
    const size_t row_bytes = static_cast<size_t>(width) * 4;
    texture_scratch.resize(row_bytes * height);
    const auto *source =
        static_cast<const uint8_t *>(texture.GetPixelsAt(x, y));
    for (uint32_t row = 0; row < height; ++row) {
      const uint8_t *source_row =
          source + static_cast<size_t>(row) * texture.GetPitch();
      uint8_t *destination_row =
          texture_scratch.data() + static_cast<size_t>(row) * row_bytes;
      for (uint32_t column = 0; column < width; ++column) {
        const uint8_t alpha = source_row[column * 4 + 3];
        destination_row[column * 4] =
            premultiply(source_row[column * 4], alpha);
        destination_row[column * 4 + 1] =
            premultiply(source_row[column * 4 + 1], alpha);
        destination_row[column * 4 + 2] =
            premultiply(source_row[column * 4 + 2], alpha);
        destination_row[column * 4 + 3] = alpha;
      }
    }
    return graphics.setTexture(handle, OOS_TEXTURE_RGBA8888, x, y, width,
                               height, static_cast<uint32_t>(row_bytes), flags,
                               texture_scratch.data(), texture_scratch.size());
  }

  bool submit(uint32_t clear_rgba) {
    if (!initialized || !frame_started)
      return false;
    ImGui::SetCurrentContext(context);
    ImGui::Render();
    frame_started = false;
    ImDrawData *draw_data = ImGui::GetDrawData();
    if (!draw_data) {
      error = "Dear ImGui did not produce draw data";
      return false;
    }
    if (draw_data->Textures) {
      for (ImTextureData *texture : *draw_data->Textures) {
        if (texture && texture->Status != ImTextureStatus_OK &&
            !updateTexture(*texture))
          return false;
      }
    }
    if (draw_data->TotalVtxCount < 0 || draw_data->TotalIdxCount < 0 ||
        draw_data->TotalVtxCount > static_cast<int>(OOS_GFX_MAX_VERTICES) ||
        draw_data->TotalIdxCount > static_cast<int>(OOS_GFX_MAX_INDICES)) {
      error = "Dear ImGui frame exceeds OOS mesh limits";
      return false;
    }
    vertices.clear();
    indices.clear();
    commands.clear();
    vertices.reserve(static_cast<size_t>(draw_data->TotalVtxCount));
    indices.reserve(static_cast<size_t>(draw_data->TotalIdxCount));
    const ImVec2 clip_offset = draw_data->DisplayPos;
    const ImVec2 clip_scale = draw_data->FramebufferScale;

    for (const ImDrawList *list : draw_data->CmdLists) {
      const uint32_t vertex_base = static_cast<uint32_t>(vertices.size());
      const uint32_t index_base = static_cast<uint32_t>(indices.size());
      for (const ImDrawVert &source : list->VtxBuffer) {
        OosGfxVertex vertex = {};
        vertex.position[0] =
            (source.pos.x - draw_data->DisplayPos.x) * clip_scale.x;
        vertex.position[1] =
            (source.pos.y - draw_data->DisplayPos.y) * clip_scale.y;
        vertex.uv[0] = source.uv.x;
        vertex.uv[1] = source.uv.y;
        const uint8_t alpha = colorChannel(source.col, IM_COL32_A_SHIFT);
        vertex.color[0] =
            premultiply(colorChannel(source.col, IM_COL32_R_SHIFT), alpha);
        vertex.color[1] =
            premultiply(colorChannel(source.col, IM_COL32_G_SHIFT), alpha);
        vertex.color[2] =
            premultiply(colorChannel(source.col, IM_COL32_B_SHIFT), alpha);
        vertex.color[3] = alpha;
        vertices.push_back(vertex);
      }
      for (const ImDrawIdx source : list->IdxBuffer)
        indices.push_back(static_cast<uint16_t>(source));

      for (const ImDrawCmd &source : list->CmdBuffer) {
        if (source.UserCallback) {
          if (source.UserCallback !=
              ImGui::GetPlatformIO().DrawCallback_ResetRenderState) {
            error = "Dear ImGui custom draw callback is unsupported";
            return false;
          }
          continue;
        }
        const float min_x = (source.ClipRect.x - clip_offset.x) * clip_scale.x;
        const float min_y = (source.ClipRect.y - clip_offset.y) * clip_scale.y;
        const float max_x = (source.ClipRect.z - clip_offset.x) * clip_scale.x;
        const float max_y = (source.ClipRect.w - clip_offset.y) * clip_scale.y;
        const float clipped_min_x = std::max(0.0f, min_x);
        const float clipped_min_y = std::max(0.0f, min_y);
        const float clipped_max_x =
            std::min(static_cast<float>(graphics.width()), max_x);
        const float clipped_max_y =
            std::min(static_cast<float>(graphics.height()), max_y);
        if (clipped_max_x <= clipped_min_x || clipped_max_y <= clipped_min_y)
          continue;
        const ImTextureID texture_id = source.GetTexID();
        if (texture_id == ImTextureID_Invalid ||
            texture_id > std::numeric_limits<uint32_t>::max()) {
          error = "Dear ImGui draw command has an invalid texture";
          return false;
        }
        const uint32_t adjusted_base = vertex_base + source.VtxOffset;
        const uint32_t first_index = index_base + source.IdxOffset;
        if (first_index + source.ElemCount > indices.size()) {
          error = "Dear ImGui draw command index range is invalid";
          return false;
        }
        for (uint32_t index = first_index;
             index < first_index + source.ElemCount; ++index) {
          const uint32_t adjusted = indices[index] + adjusted_base;
          if (adjusted > std::numeric_limits<uint16_t>::max()) {
            error = "Dear ImGui frame requires 32-bit indices";
            return false;
          }
          indices[index] = static_cast<uint16_t>(adjusted);
        }
        commands.push_back({first_index,
                            source.ElemCount,
                            static_cast<uint32_t>(texture_id),
                            {clipped_min_x, clipped_min_y},
                            {clipped_max_x, clipped_max_y}});
      }
    }
    if (commands.size() > OOS_GFX_MAX_DRAW_COMMANDS) {
      error = "Dear ImGui frame exceeds OOS draw-command limits";
      return false;
    }
    if (!graphics.submit(vertices.data(), vertices.size(), indices.data(),
                         indices.size(), commands.data(), commands.size(),
                         clear_rgba)) {
      error = "Dear ImGui frame submission failed";
      return false;
    }
    error.clear();
    return true;
  }

  runtime::GraphicsHost &graphics;
  ImGuiContext *context = nullptr;
  std::vector<OosGfxVertex> vertices;
  std::vector<uint16_t> indices;
  std::vector<OosGfxDrawCommand> commands;
  std::vector<uint8_t> texture_scratch;
  std::string error;
  int64_t last_frame_us = 0;
  uint32_t next_texture = kFirstTextureHandle;
  bool initialized = false;
  bool frame_started = false;
};

ImguiBackend::ImguiBackend(runtime::GraphicsHost &graphics)
    : impl_(std::make_unique<Impl>(graphics)) {}

ImguiBackend::~ImguiBackend() { shutdown(); }

bool ImguiBackend::initialize() { return impl_->initialize(); }

void ImguiBackend::shutdown() {
  if (impl_)
    impl_->shutdown();
}

bool ImguiBackend::dispatchKey(const input::KeyEvent &event) {
  return impl_->dispatchKey(event);
}

bool ImguiBackend::beginFrame(int64_t monotonic_us) {
  return impl_->beginFrame(monotonic_us);
}

bool ImguiBackend::submit(uint32_t clear_rgba) {
  return impl_->submit(clear_rgba);
}

ImGuiContext *ImguiBackend::context() const { return impl_->context; }

const std::string &ImguiBackend::lastError() const { return impl_->error; }

} // namespace oos::sdk::ui
