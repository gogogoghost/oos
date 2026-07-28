#include "oos/input/key_input.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {

struct EventCount {
  int value = 0;
};

void printEvent(void *context, const oos::input::KeyEvent &event) {
  auto *count = static_cast<EventCount *>(context);
  ++count->value;
  std::printf("key code=%u name=%s action=%s device=%s path=%s\n", event.code,
              oos::input::keyCodeName(event.code),
              oos::input::keyActionName(event.action),
              event.device_name.c_str(), event.device_path.c_str());
  std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv) {
  const int seconds = argc == 2 ? std::atoi(argv[1]) : 30;
  if (seconds < 1 || seconds > 120) {
    std::fprintf(stderr, "usage: %s [1-120 seconds]\n", argv[0]);
    return 2;
  }

  oos::input::KeyInput input;
  if (!input.initialize())
    return 1;
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
  std::printf("key_event_count=%d\n", count.value);
  return 0;
}
