#pragma once

#include "oos/compositor/compositor.h"
#include "oos/runtime/application_session.h"
#include "oos/ui/status_bar_appearance.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace oos::runtime {

class ApplicationSessionManager final : public window::ApplicationInputTarget {
public:
  using Factory = std::function<std::unique_ptr<ApplicationSession>(
      GraphicsHost &, ui::StatusBarAppearanceController &)>;

  ApplicationSessionManager(compositor::Compositor &compositor, int32_t x,
                            int32_t y, uint32_t width, uint32_t height,
                            ui::StatusBarAppearanceHost &appearance_host,
                            ui::StatusBarAppearance default_appearance);
  ~ApplicationSessionManager();

  ApplicationSessionManager(const ApplicationSessionManager &) = delete;
  ApplicationSessionManager &
  operator=(const ApplicationSessionManager &) = delete;

  bool registerFactory(std::string id, Factory factory);
  bool registered(const std::string &id) const;
  bool activate(const std::string &id);
  bool frame(int64_t monotonic_us, uint32_t &next_delay_ms);
  std::string takeLaunchRequest();
  void shutdown();

  bool dispatchKey(const input::KeyEvent &event, int64_t monotonic_us) override;
  const char *lastError() const override;
  const char *activeId() const;
  size_t residentCount() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::runtime
