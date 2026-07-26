#include "oos/input/key_input.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

namespace oos::input {
namespace {

constexpr size_t kBitsPerWord = sizeof(unsigned long) * 8;
constexpr size_t kEventTypeWords = (EV_MAX / kBitsPerWord) + 1;
constexpr size_t kReadyEventCount = 16;
constexpr size_t kReadEventCount = 32;

bool bitIsSet(const unsigned long *bits, size_t bit) {
  return (bits[bit / kBitsPerWord] & (1UL << (bit % kBitsPerWord))) != 0;
}

bool isEventNode(const char *name) {
  constexpr char kPrefix[] = "event";
  if (std::strncmp(name, kPrefix, sizeof(kPrefix) - 1) != 0)
    return false;
  const char *digit = name + sizeof(kPrefix) - 1;
  if (*digit == '\0')
    return false;
  for (; *digit; ++digit) {
    if (*digit < '0' || *digit > '9')
      return false;
  }
  return true;
}

} // namespace

struct KeyInput::Implementation {
  struct Device {
    int fd = -1;
    KeyDeviceInfo info;
    bool grabbed = false;
  };

  explicit Implementation(KeyInputOptions requested_options)
      : options(requested_options) {}

  KeyInputOptions options;
  int epoll_fd = -1;
  std::vector<Device> devices;
  std::vector<KeyDeviceInfo> public_devices;
};

KeyInput::KeyInput(KeyInputOptions options)
    : implementation_(std::make_unique<Implementation>(options)) {}

KeyInput::~KeyInput() { shutdown(); }

bool KeyInput::initialize(const char *input_directory) {
  shutdown();
  if (!input_directory || input_directory[0] == '\0') {
    errno = EINVAL;
    return false;
  }

  DIR *directory = opendir(input_directory);
  if (!directory) {
    std::fprintf(stderr, "open input directory %s failed: %s\n",
                 input_directory, std::strerror(errno));
    return false;
  }

  std::vector<std::string> paths;
  while (dirent *entry = readdir(directory)) {
    if (isEventNode(entry->d_name))
      paths.emplace_back(std::string(input_directory) + "/" + entry->d_name);
  }
  closedir(directory);
  std::sort(paths.begin(), paths.end());

  implementation_->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (implementation_->epoll_fd < 0) {
    std::fprintf(stderr, "epoll_create1 for key input failed: %s\n",
                 std::strerror(errno));
    return false;
  }

  for (const std::string &path : paths) {
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      std::fprintf(stderr, "open key input %s failed: %s\n", path.c_str(),
                   std::strerror(errno));
      continue;
    }

    std::array<unsigned long, kEventTypeWords> event_types{};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(event_types)), event_types.data()) < 0 ||
        !bitIsSet(event_types.data(), EV_KEY)) {
      close(fd);
      continue;
    }

    std::array<char, 256> name{};
    if (ioctl(fd, EVIOCGNAME(name.size()), name.data()) < 0)
      std::snprintf(name.data(), name.size(), "unknown");

    bool grabbed = false;
    if (implementation_->options.grab_devices) {
      if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        std::fprintf(stderr, "grab key input %s (%s) failed: %s\n",
                     path.c_str(), name.data(), std::strerror(errno));
        close(fd);
        continue;
      }
      grabbed = true;
    }

    const uint32_t index = implementation_->devices.size();
    epoll_event watch{};
    watch.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    watch.data.u32 = index;
    if (epoll_ctl(implementation_->epoll_fd, EPOLL_CTL_ADD, fd, &watch) < 0) {
      std::fprintf(stderr, "watch key input %s failed: %s\n", path.c_str(),
                   std::strerror(errno));
      if (grabbed)
        ioctl(fd, EVIOCGRAB, 0);
      close(fd);
      continue;
    }

    Implementation::Device device;
    device.fd = fd;
    device.info = {path, name.data()};
    device.grabbed = grabbed;
    implementation_->public_devices.push_back(device.info);
    implementation_->devices.push_back(std::move(device));
  }

  if (!implementation_->devices.empty())
    return true;

  std::fprintf(stderr, "no EV_KEY devices found under %s\n", input_directory);
  shutdown();
  return false;
}

void KeyInput::shutdown() {
  if (!implementation_)
    return;
  for (Implementation::Device &device : implementation_->devices) {
    if (device.grabbed)
      ioctl(device.fd, EVIOCGRAB, 0);
    if (device.fd >= 0)
      close(device.fd);
  }
  implementation_->devices.clear();
  implementation_->public_devices.clear();
  if (implementation_->epoll_fd >= 0) {
    close(implementation_->epoll_fd);
    implementation_->epoll_fd = -1;
  }
}

int KeyInput::poll(int timeout_ms, KeyEventCallback callback, void *context) {
  if (!initialized()) {
    errno = ENODEV;
    return -1;
  }

  std::array<epoll_event, kReadyEventCount> ready{};
  const int ready_count = epoll_wait(implementation_->epoll_fd, ready.data(),
                                     ready.size(), timeout_ms);
  if (ready_count < 0) {
    if (errno == EINTR)
      return 0;
    std::fprintf(stderr, "key input epoll_wait failed: %s\n",
                 std::strerror(errno));
    return -1;
  }

  int dispatched = 0;
  for (int ready_index = 0; ready_index < ready_count; ++ready_index) {
    const uint32_t device_index = ready[ready_index].data.u32;
    if (device_index >= implementation_->devices.size())
      continue;
    const Implementation::Device &device =
        implementation_->devices[device_index];

    while (true) {
      std::array<input_event, kReadEventCount> events{};
      const ssize_t bytes = read(device.fd, events.data(), sizeof(events));
      if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        break;
      if (bytes < 0) {
        std::fprintf(stderr, "read key input %s failed: %s\n",
                     device.info.path.c_str(), std::strerror(errno));
        return -1;
      }
      if (bytes == 0)
        break;
      if (bytes % sizeof(input_event) != 0) {
        std::fprintf(stderr, "short evdev record from %s\n",
                     device.info.path.c_str());
        errno = EIO;
        return -1;
      }

      const size_t event_count = bytes / sizeof(input_event);
      for (size_t event_index = 0; event_index < event_count; ++event_index) {
        const input_event &raw = events[event_index];
        if (raw.type != EV_KEY || raw.value < 0 || raw.value > 2)
          continue;
        KeyEvent event;
        event.timestamp_us =
            static_cast<int64_t>(raw.time.tv_sec) * 1000000 + raw.time.tv_usec;
        event.code = raw.code;
        event.action = static_cast<KeyAction>(raw.value);
        event.device_path = device.info.path;
        event.device_name = device.info.name;
        if (callback)
          callback(context, event);
        ++dispatched;
      }
    }
  }
  return dispatched;
}

bool KeyInput::initialized() const {
  return implementation_ && implementation_->epoll_fd >= 0 &&
         !implementation_->devices.empty();
}

int KeyInput::fileDescriptor() const {
  return implementation_ ? implementation_->epoll_fd : -1;
}

const std::vector<KeyDeviceInfo> &KeyInput::devices() const {
  return implementation_->public_devices;
}

const char *keyActionName(KeyAction action) {
  switch (action) {
  case KeyAction::Released:
    return "released";
  case KeyAction::Pressed:
    return "pressed";
  case KeyAction::Repeated:
    return "repeated";
  }
  return "unknown";
}

const char *keyCodeName(uint16_t code) {
  switch (code) {
  case 2:
    return "KEY_1";
  case 3:
    return "KEY_2";
  case 4:
    return "KEY_3";
  case 5:
    return "KEY_4";
  case 6:
    return "KEY_5";
  case 7:
    return "KEY_6";
  case 8:
    return "KEY_7";
  case 9:
    return "KEY_8";
  case 10:
    return "KEY_9";
  case 11:
    return "KEY_0";
  case 103:
    return "KEY_UP";
  case 105:
    return "KEY_LEFT";
  case 106:
    return "KEY_RIGHT";
  case 108:
    return "KEY_DOWN";
  case 114:
    return "KEY_VOLUMEDOWN";
  case 115:
    return "KEY_VOLUMEUP";
  case 116:
    return "KEY_POWER";
  case 139:
    return "KEY_MENU";
  case 148:
    return "KEY_PROG1";
  case 158:
    return "KEY_BACK";
  case 231:
    return "KEY_SEND";
  case 249:
    return "HALL_SENSOR";
  case 352:
    return "KEY_OK";
  case 357:
    return "KEY_OPTION";
  case 358:
    return "KEY_INFO";
  case 522:
    return "KEY_NUMERIC_STAR";
  case 523:
    return "KEY_NUMERIC_POUND";
  default:
    return "KEY_UNKNOWN";
  }
}

} // namespace oos::input
