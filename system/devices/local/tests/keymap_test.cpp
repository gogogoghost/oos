#include "oos/local/local_key_input.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdio>

namespace {

struct Received {
  std::array<uint16_t, 6> codes{};
  size_t count = 0;
};

void receive(void *context, const oos::input::KeyEvent &event) {
  auto *received = static_cast<Received *>(context);
  if (event.action == oos::input::KeyAction::Pressed &&
      received->count < received->codes.size()) {
    received->codes[received->count++] = event.code;
  }
}

bool push(SDL_Keycode key) {
  SDL_Event event{};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.type = SDL_EVENT_KEY_DOWN;
  event.key.key = key;
  return SDL_PushEvent(&event);
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2 || !SDL_Init(SDL_INIT_EVENTS))
    return 2;
  oos::local::LocalKeyInput input;
  if (!input.initialize(argv[1])) {
    std::fprintf(stderr, "%s\n", input.lastError());
    return 1;
  }
  if (!push(SDLK_1) || !push(SDLK_0) || !push(SDLK_RETURN) ||
      !push(SDLK_BACKSPACE) || !push(SDLK_Q) || !push(SDLK_W)) {
    return 1;
  }
  Received received;
  if (input.poll(0, receive, &received) != 6 || received.count != 6)
    return 1;
  constexpr std::array<uint16_t, 6> expected = {2, 11, 352, 158, 139, 357};
  if (received.codes != expected)
    return 1;
  input.shutdown();
  SDL_Quit();
  std::puts("local keymap passed");
  return 0;
}
