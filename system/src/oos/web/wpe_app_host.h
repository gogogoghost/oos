#pragma once

#include "oos/apps/app_repository.h"

#include <csignal>
#include <string>

namespace oos::compositor {
class Compositor;
}

namespace oos::device {
struct DeviceDescriptor;
}

namespace oos::input {
class KeyInputSource;
}

namespace oos::web {

// Owns the foreground WPE producer lifecycle while keeping display and input
// authority in the OOS host process.
class WpeAppHost {
public:
  WpeAppHost(compositor::Compositor &compositor, input::KeyInputSource &input);

  bool run(const apps::AppLaunch &launch,
           const device::DeviceDescriptor &device,
           volatile std::sig_atomic_t *stop_requested);

  const std::string &lastError() const { return error_; }

private:
  compositor::Compositor &compositor_;
  input::KeyInputSource &input_;
  std::string error_;
};

} // namespace oos::web
