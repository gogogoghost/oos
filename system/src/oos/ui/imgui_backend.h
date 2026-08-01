#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <imgui.h>

#include "oos/input/key_input.h"

namespace oos::runtime {
class GraphicsHost;
}

namespace oos::ui {

// Dear ImGui renderer/platform backend for the OOS compositor contract. UI
// code builds an ImGui frame between beginFrame() and submit().
class ImguiBackend {
public:
  explicit ImguiBackend(runtime::GraphicsHost &graphics);
  ~ImguiBackend();

  ImguiBackend(const ImguiBackend &) = delete;
  ImguiBackend &operator=(const ImguiBackend &) = delete;

  bool initialize();
  void shutdown();
  bool dispatchKey(const input::KeyEvent &event);
  bool beginFrame(int64_t monotonic_us);
  bool submit(uint32_t clear_rgba = 0xff000000u);

  ImGuiContext *context() const;
  const std::string &lastError() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::ui
