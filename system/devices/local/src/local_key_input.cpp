#include "oos/local/local_key_input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace oos::local {
namespace {

struct NamedKey {
  const char *name;
  uint16_t code;
};

constexpr std::array<NamedKey, 24> kOosKeys = {{
    {"KEY_1", 2},
    {"KEY_2", 3},
    {"KEY_3", 4},
    {"KEY_4", 5},
    {"KEY_5", 6},
    {"KEY_6", 7},
    {"KEY_7", 8},
    {"KEY_8", 9},
    {"KEY_9", 10},
    {"KEY_0", 11},
    {"KEY_UP", 103},
    {"KEY_LEFT", 105},
    {"KEY_RIGHT", 106},
    {"KEY_DOWN", 108},
    {"KEY_VOLUMEDOWN", 114},
    {"KEY_VOLUMEUP", 115},
    {"KEY_POWER", 116},
    {"KEY_MENU", 139},
    {"KEY_PROG1", 148},
    {"KEY_BACK", 158},
    {"KEY_OK", 352},
    {"KEY_OPTION", 357},
    {"KEY_NUMERIC_STAR", 522},
    {"KEY_NUMERIC_POUND", 523},
}};

char *trim(char *value) {
  while (*value == ' ' || *value == '\t')
    ++value;
  char *end = value + std::strlen(value);
  while (end != value && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\r' || end[-1] == '\n')) {
    --end;
  }
  *end = '\0';
  return value;
}

bool parseOosKey(const char *name, uint16_t &code) {
  for (const NamedKey &key : kOosKeys) {
    if (std::strcmp(name, key.name) == 0) {
      code = key.code;
      return true;
    }
  }
  return false;
}

} // namespace

class LocalKeyInput::Impl {
public:
  struct Mapping {
    SDL_Keycode host;
    uint16_t oos;
  };

  bool initialize(const char *path) {
    shutdown();
    if (!path || !path[0]) {
      error = "local keymap path is empty";
      return false;
    }
    FILE *file = std::fopen(path, "r");
    if (!file) {
      error = std::string("open local keymap ") + path + ": " +
              std::strerror(errno);
      return false;
    }

    std::array<char, 512> line{};
    unsigned line_number = 0;
    while (std::fgets(line.data(), line.size(), file)) {
      ++line_number;
      char *entry = trim(line.data());
      if (!entry[0] || entry[0] == '#')
        continue;
      char *separator = std::strchr(entry, '=');
      if (!separator) {
        error = std::string("invalid local keymap line ") +
                std::to_string(line_number);
        std::fclose(file);
        mappings.clear();
        return false;
      }
      *separator = '\0';
      const char *host_name = trim(entry);
      const char *oos_name = trim(separator + 1);
      const SDL_Keycode host_key = SDL_GetKeyFromName(host_name);
      uint16_t oos_key = 0;
      if (host_key == SDLK_UNKNOWN || !parseOosKey(oos_name, oos_key)) {
        error = std::string("unknown key on local keymap line ") +
                std::to_string(line_number) + ": " + host_name + " = " +
                oos_name;
        std::fclose(file);
        mappings.clear();
        return false;
      }
      const auto duplicate =
          std::find_if(mappings.begin(), mappings.end(),
                       [host_key](Mapping m) { return m.host == host_key; });
      if (duplicate == mappings.end())
        mappings.push_back({host_key, oos_key});
      else
        duplicate->oos = oos_key;
    }
    std::fclose(file);
    if (mappings.empty()) {
      error = "local keymap contains no mappings";
      return false;
    }
    std::sort(
        mappings.begin(), mappings.end(),
        [](Mapping left, Mapping right) { return left.host < right.host; });
    public_devices.push_back({path, "SDL keyboard"});
    initialized = true;
    error.clear();
    std::fprintf(stderr, "OOS local keymap loaded: %zu mappings from %s\n",
                 mappings.size(), path);
    return true;
  }

  void shutdown() {
    mappings.clear();
    public_devices.clear();
    initialized = false;
    stop_requested = false;
  }

  const Mapping *find(SDL_Keycode key) const {
    const auto found = std::lower_bound(mappings.begin(), mappings.end(), key,
                                        [](Mapping mapping, SDL_Keycode value) {
                                          return mapping.host < value;
                                        });
    return found != mappings.end() && found->host == key ? &*found : nullptr;
  }

  int dispatch(const SDL_Event &raw, input::KeyEventCallback callback,
               void *context) {
    if (raw.type == SDL_EVENT_QUIT ||
        raw.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
      stop_requested = true;
      return 0;
    }
    if (raw.type != SDL_EVENT_KEY_DOWN && raw.type != SDL_EVENT_KEY_UP)
      return 0;
    const Mapping *mapping = find(raw.key.key);
    if (!mapping)
      return 0;
    input::KeyEvent event;
    event.timestamp_us = static_cast<int64_t>(raw.key.timestamp / 1000);
    event.code = mapping->oos;
    event.action = raw.type == SDL_EVENT_KEY_UP ? input::KeyAction::Released
                   : raw.key.repeat             ? input::KeyAction::Repeated
                                                : input::KeyAction::Pressed;
    event.device_path = public_devices[0].path;
    event.device_name = public_devices[0].name;
    if (callback)
      callback(context, event);
    return 1;
  }

  std::vector<Mapping> mappings;
  std::vector<input::KeyDeviceInfo> public_devices;
  std::string error;
  bool initialized = false;
  bool stop_requested = false;
};

LocalKeyInput::LocalKeyInput() : impl_(std::make_unique<Impl>()) {}
LocalKeyInput::~LocalKeyInput() { shutdown(); }

bool LocalKeyInput::initialize(const char *mapping_path) {
  return impl_->initialize(mapping_path);
}

void LocalKeyInput::shutdown() {
  if (impl_)
    impl_->shutdown();
}

int LocalKeyInput::poll(int timeout_ms, input::KeyEventCallback callback,
                        void *context) {
  if (!initialized()) {
    errno = ENODEV;
    return -1;
  }
  SDL_Event event;
  int dispatched = 0;
  if (timeout_ms > 0 && SDL_WaitEventTimeout(&event, timeout_ms))
    dispatched += impl_->dispatch(event, callback, context);
  while (SDL_PollEvent(&event))
    dispatched += impl_->dispatch(event, callback, context);
  return dispatched;
}

bool LocalKeyInput::initialized() const { return impl_ && impl_->initialized; }

int LocalKeyInput::fileDescriptor() const { return -1; }

const std::vector<input::KeyDeviceInfo> &LocalKeyInput::devices() const {
  return impl_->public_devices;
}

bool LocalKeyInput::stopRequested() const {
  return impl_ && impl_->stop_requested;
}

const char *LocalKeyInput::lastError() const { return impl_->error.c_str(); }

} // namespace oos::local
