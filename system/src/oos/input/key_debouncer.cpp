#include "oos/input/key_input.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace oos::input {

struct KeyDebouncer::Implementation {
  struct State {
    std::string device_path;
    std::string device_name;
    uint16_t code = 0;
    bool down = false;
    bool release_pending = false;
    int64_t release_timestamp_us = 0;
  };

  explicit Implementation(int64_t requested_interval_us)
      : interval_us(std::max<int64_t>(0, requested_interval_us)) {}

  State &stateFor(const KeyEvent &event) {
    const auto existing =
        std::find_if(states.begin(), states.end(), [&](const State &state) {
          return state.code == event.code &&
                 state.device_path == event.device_path;
        });
    if (existing != states.end())
      return *existing;
    states.push_back({std::string(event.device_path),
                      std::string(event.device_name), event.code});
    return states.back();
  }

  int64_t interval_us;
  std::vector<State> states;
};

KeyDebouncer::KeyDebouncer(int64_t interval_us)
    : implementation_(std::make_unique<Implementation>(interval_us)) {}

KeyDebouncer::~KeyDebouncer() = default;

int KeyDebouncer::flush(int64_t timestamp_us, KeyEventCallback callback,
                        void *context) {
  int dispatched = 0;
  while (true) {
    Implementation::State *next = nullptr;
    int64_t next_deadline = std::numeric_limits<int64_t>::max();
    for (Implementation::State &state : implementation_->states) {
      if (!state.release_pending)
        continue;
      const int64_t deadline =
          state.release_timestamp_us + implementation_->interval_us;
      if (deadline <= timestamp_us && deadline < next_deadline) {
        next = &state;
        next_deadline = deadline;
      }
    }
    if (!next)
      break;

    const KeyEvent release = {next_deadline, next->code, KeyAction::Released,
                              next->device_path, next->device_name};
    if (callback)
      callback(context, release);
    next->release_pending = false;
    next->down = false;
    ++dispatched;
  }
  return dispatched;
}

int KeyDebouncer::process(const KeyEvent &event, KeyEventCallback callback,
                          void *context) {
  if (implementation_->interval_us == 0) {
    if (callback)
      callback(context, event);
    return 1;
  }

  int dispatched = flush(event.timestamp_us, callback, context);
  Implementation::State &state = implementation_->stateFor(event);
  switch (event.action) {
  case KeyAction::Pressed:
    if (state.release_pending) {
      state.release_pending = false;
      return dispatched;
    }
    if (state.down)
      return dispatched;
    state.down = true;
    if (callback)
      callback(context, event);
    return dispatched + 1;
  case KeyAction::Released:
    if (!state.down)
      return dispatched;
    state.release_pending = true;
    state.release_timestamp_us = event.timestamp_us;
    return dispatched;
  case KeyAction::Repeated:
    if (!state.down)
      return dispatched;
    state.release_pending = false;
    if (callback)
      callback(context, event);
    return dispatched + 1;
  }
  return dispatched;
}

int64_t KeyDebouncer::nextDeadlineUs() const {
  int64_t deadline = std::numeric_limits<int64_t>::max();
  for (const Implementation::State &state : implementation_->states) {
    if (state.release_pending)
      deadline = std::min(deadline, state.release_timestamp_us +
                                        implementation_->interval_us);
  }
  return deadline == std::numeric_limits<int64_t>::max() ? -1 : deadline;
}

void KeyDebouncer::reset() { implementation_->states.clear(); }

} // namespace oos::input
