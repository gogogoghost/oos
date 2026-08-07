#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include <unistd.h>

#include "oos/nokia2780/primary_gles_display.h"
#include "oos/runtime/wasm_app.h"

int main(int argc, char **argv) {
  if (argc < 2 || argc > 4) {
    std::fprintf(stderr,
                 "usage: %s WASM_BASE [INSTANCE_COUNT=3] [SECONDS=20]\n",
                 argv[0]);
    return 2;
  }
  const int instance_count = argc >= 3 ? std::atoi(argv[2]) : 3;
  const int seconds = argc >= 4 ? std::atoi(argv[3]) : 20;
  if (instance_count < 1 || instance_count > 8 || seconds < 1 || seconds > 300)
    return 2;

  oos::nokia2780::PrimaryGlesDisplay display;
  if (!display.initialize())
    return 1;

  std::vector<std::unique_ptr<oos::runtime::WasmApp>> apps;
  for (int index = 0; index < instance_count; ++index) {
    auto app = std::make_unique<oos::runtime::WasmApp>(display);
    if (!app->load(argv[1]) || !app->initialize()) {
      std::fprintf(stderr, "instance %d failed: %s\n", index, app->lastError());
      return 1;
    }
    oos::input::KeyEvent key;
    key.action = oos::input::KeyAction::Pressed;
    if (index >= 1) {
      key.code = 352;
      if (!app->dispatchKey(key, 1'000'000))
        return 1;
    }
    if (index >= 2) {
      key.code = 106;
      if (!app->dispatchKey(key, 1'010'000))
        return 1;
    }
    if (!app->render(1'020'000 + index * 10'000)) {
      std::fprintf(stderr, "instance %d initial render failed: %s\n", index,
                   app->lastError());
      return 1;
    }
    apps.push_back(std::move(app));
  }

  std::fprintf(stderr, "OOS_WASM_MULTI_READY pid=%d instances=%zu\n", getpid(),
               apps.size());
  std::fflush(stderr);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  size_t active = 0;
  auto next_switch = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_switch) {
      active = (active + 1) % apps.size();
      next_switch = now + std::chrono::seconds(1);
      std::fprintf(stderr, "OOS_WASM_MULTI_ACTIVE index=%zu\n", active);
    }
    if (!apps[active]->render(2'000'000)) {
      std::fprintf(stderr, "foreground render failed: %s\n",
                   apps[active]->lastError());
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  apps.clear();
  display.shutdown();
  return 0;
}
