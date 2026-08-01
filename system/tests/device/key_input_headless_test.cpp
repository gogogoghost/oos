#include "oos/input/key_input.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct EventCount {
  int value = 0;
  int64_t previous_timestamp_us = 0;
};

void printEvent(void *context, const oos::input::KeyEvent &event) {
  auto *count = static_cast<EventCount *>(context);
  ++count->value;
  const int64_t delta_us =
      count->previous_timestamp_us == 0
          ? 0
          : event.timestamp_us - count->previous_timestamp_us;
  count->previous_timestamp_us = event.timestamp_us;
  std::printf(
      "event=%d timestamp_us=%lld delta_us=%lld key code=%u name=%s "
      "action=%s device=%.*s path=%.*s\n",
      count->value, static_cast<long long>(event.timestamp_us),
      static_cast<long long>(delta_us), event.code,
      oos::input::keyCodeName(event.code),
      oos::input::keyActionName(event.action),
      static_cast<int>(event.device_name.size()), event.device_name.data(),
      static_cast<int>(event.device_path.size()), event.device_path.data());
  std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv) {
  const int seconds = argc >= 2 ? std::atoi(argv[1]) : 30;
  const char *mode = argc >= 3 ? argv[2] : "raw";
  const bool raw_mode = std::strcmp(mode, "raw") == 0;
  const bool debounced_mode = std::strcmp(mode, "debounced") == 0;
  if (argc > 3 || seconds < 1 || seconds > 120 ||
      (!raw_mode && !debounced_mode)) {
    std::fprintf(stderr, "usage: %s [1-120 seconds] [raw|debounced]\n",
                 argv[0]);
    return 2;
  }

  constexpr int64_t kDebounceIntervalUs = 30000;
  oos::input::KeyInput input({false, raw_mode ? 0 : kDebounceIntervalUs});
  if (!input.initialize())
    return 1;
  std::printf("mode=%s debounce_interval_us=%lld\n", mode,
              static_cast<long long>(raw_mode ? 0 : kDebounceIntervalUs));
  for (const auto &device : input.devices())
    std::printf("device name=%s path=%s\n", device.name.c_str(),
                device.path.c_str());
  std::fflush(stdout);

  EventCount count;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (input.poll(250, printEvent, &count) < 0)
      return 1;
  }

  // Keep both raw and filtered diagnostics alive through one full debounce
  // window so a release at the collection boundary is not omitted.
  const auto drain_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  while (std::chrono::steady_clock::now() < drain_deadline) {
    if (input.poll(10, printEvent, &count) < 0)
      return 1;
  }
  std::printf("key_event_count=%d\n", count.value);
  return 0;
}
