#pragma once

#include "oos/apps/app_repository.h"

#include <csignal>
#include <memory>
#include <string>

namespace oos::compositor {
class Compositor;
}

namespace oos::device {
class Device;
class ServiceProvider;
}

namespace oos::input {
class KeyInputSource;
}

namespace oos::web {

// Owns the foreground WPE producer lifecycle while keeping display and input
// authority in the OOS host process.
class WpeAppHost {
public:
  WpeAppHost(compositor::Compositor &compositor, input::KeyInputSource &input,
             device::Device &device);
  ~WpeAppHost();

  bool run(const apps::AppLaunch &launch,
           volatile std::sig_atomic_t *stop_requested);

  const std::string &lastError() const { return error_; }

private:
  compositor::Compositor &compositor_;
  input::KeyInputSource &input_;
  device::Device &device_;
  std::unique_ptr<device::ServiceProvider> services_;
  std::string error_;
};

} // namespace oos::web
