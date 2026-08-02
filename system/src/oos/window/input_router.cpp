#include "oos/window/input_router.h"

namespace oos::window {

InputRouter::InputRouter(SystemInputTarget &system_ui,
                         ApplicationInputTarget &app)
    : system_ui_(system_ui), app_(&app) {}

void InputRouter::setApplicationTarget(ApplicationInputTarget &app) {
  app_ = &app;
}

bool InputRouter::dispatch(const input::KeyEvent &event, int64_t monotonic_us) {
  error_.clear();
  bool consumed = false;
  if (!system_ui_.routeKey(event, consumed)) {
    error_ = system_ui_.lastError();
    return false;
  }
  if (consumed)
    return true;
  if (!app_->dispatchKey(event, monotonic_us)) {
    error_ = app_->lastError();
    return false;
  }
  return true;
}

const std::string &InputRouter::lastError() const { return error_; }

} // namespace oos::window
