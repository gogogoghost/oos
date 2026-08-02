#pragma once

#include <cstdint>
#include <string>

#include "oos/input/key_input.h"

namespace oos::window {

class SystemInputTarget {
public:
  virtual ~SystemInputTarget() = default;
  virtual bool routeKey(const input::KeyEvent &event, bool &consumed) = 0;
  virtual const std::string &lastError() const = 0;
};

class ApplicationInputTarget {
public:
  virtual ~ApplicationInputTarget() = default;
  virtual bool dispatchKey(const input::KeyEvent &event,
                           int64_t monotonic_us) = 0;
  virtual const char *lastError() const = 0;
};

// System overlays get first refusal. Non-modal shell UI leaves input with the
// active application; lock screen and modal overlays consume it globally.
class InputRouter {
public:
  InputRouter(SystemInputTarget &system_ui, ApplicationInputTarget &app);

  void setApplicationTarget(ApplicationInputTarget &app);
  bool dispatch(const input::KeyEvent &event, int64_t monotonic_us);
  const std::string &lastError() const;

private:
  SystemInputTarget &system_ui_;
  ApplicationInputTarget *app_;
  std::string error_;
};

} // namespace oos::window
