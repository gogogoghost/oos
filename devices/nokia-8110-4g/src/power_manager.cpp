#include "oos/hardware/power_manager.h"

#include <hardware/hardware.h>
#include <hardware/power.h>
#include <suspend/autosuspend.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <linux/rtc.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <vector>

namespace oos::hardware {
namespace {

constexpr const char *kBatteryPath = "/sys/class/power_supply/battery";
constexpr const char *kUsbPath = "/sys/class/power_supply/usb";
constexpr const char *kWakeLockPath = "/sys/power/wake_lock";
constexpr const char *kWakeUnlockPath = "/sys/power/wake_unlock";
constexpr const char *kPowerStatePath = "/sys/power/state";
constexpr const char *kRtcPath = "/dev/rtc0";
constexpr uint16_t kFlipKeyCode = 249;
constexpr size_t kBitsPerWord = sizeof(unsigned long) * 8;

bool readText(const std::string &path, std::string &value) {
  value.clear();
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  std::array<char, 128> buffer{};
  const ssize_t size = read(fd, buffer.data(), buffer.size() - 1);
  const int saved_errno = errno;
  close(fd);
  errno = saved_errno;
  if (size < 0)
    return false;
  value.assign(buffer.data(), static_cast<size_t>(size));
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r' || value.back() == ' '))
    value.pop_back();
  return true;
}

bool readInteger(const std::string &path, int &value) {
  std::string text;
  if (!readText(path, text))
    return false;
  char *end = nullptr;
  errno = 0;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0') {
    errno = EINVAL;
    return false;
  }
  value = static_cast<int>(parsed);
  return true;
}

bool writeText(const char *path, const std::string &value) {
  const int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  size_t written = 0;
  while (written < value.size()) {
    const ssize_t count =
        write(fd, value.data() + written, value.size() - written);
    if (count <= 0) {
      const int saved_errno = count == 0 ? EIO : errno;
      close(fd);
      errno = saved_errno;
      return false;
    }
    written += static_cast<size_t>(count);
  }
  return close(fd) == 0;
}

BatteryState parseBatteryState(const std::string &value) {
  if (value == "Charging")
    return BatteryState::Charging;
  if (value == "Discharging")
    return BatteryState::Discharging;
  if (value == "Not charging")
    return BatteryState::NotCharging;
  if (value == "Full")
    return BatteryState::Full;
  return BatteryState::Unknown;
}

bool isEventNode(const char *name) {
  if (std::strncmp(name, "event", 5) != 0 || name[5] == '\0')
    return false;
  for (const char *cursor = name + 5; *cursor; ++cursor) {
    if (*cursor < '0' || *cursor > '9')
      return false;
  }
  return true;
}

} // namespace

struct PowerManager::Implementation {
  std::string error;
  power_module_t *power = nullptr;
  int uevent_fd = -1;
  std::set<std::string> wake_locks;
  bool autosuspend_enabled = false;
};

PowerManager::PowerManager()
    : implementation_(std::make_unique<Implementation>()) {}

PowerManager::~PowerManager() { shutdown(); }

bool PowerManager::initialize(const std::string &service_name) {
  (void)service_name;
  shutdown();
  const hw_module_t *module = nullptr;
  const int load_result = hw_get_module(POWER_HARDWARE_MODULE_ID, &module);
  if (load_result != 0 || !module) {
    implementation_->error =
        "load power HAL failed: " + std::to_string(load_result);
    return false;
  }
  implementation_->power =
      reinterpret_cast<power_module_t *>(const_cast<hw_module_t *>(module));
  if (implementation_->power->init)
    implementation_->power->init(implementation_->power);

  implementation_->uevent_fd =
      socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
  if (implementation_->uevent_fd < 0) {
    implementation_->error = "create power uevent socket failed: " +
                             std::string(std::strerror(errno));
    shutdown();
    return false;
  }
  sockaddr_nl address{};
  address.nl_family = AF_NETLINK;
  address.nl_pid = static_cast<uint32_t>(getpid());
  address.nl_groups = 1;
  if (bind(implementation_->uevent_fd, reinterpret_cast<sockaddr *>(&address),
           sizeof(address)) < 0) {
    implementation_->error =
        "bind power uevent socket failed: " + std::string(std::strerror(errno));
    shutdown();
    return false;
  }
  implementation_->error.clear();
  return true;
}

void PowerManager::shutdown() {
  if (!implementation_)
    return;
  if (implementation_->autosuspend_enabled)
    disableAutoSuspend();
  const std::vector<std::string> locks(implementation_->wake_locks.begin(),
                                       implementation_->wake_locks.end());
  for (const std::string &name : locks)
    releaseWakeLock(name);
  if (implementation_->uevent_fd >= 0) {
    close(implementation_->uevent_fd);
    implementation_->uevent_fd = -1;
  }
  implementation_->power = nullptr;
}

bool PowerManager::initialized() const {
  return implementation_->power != nullptr && implementation_->uevent_fd >= 0;
}

bool PowerManager::queryBattery(BatterySnapshot &snapshot) {
  snapshot = {};
  std::string status;
  if (!readText(std::string(kBatteryPath) + "/status", status) ||
      !readInteger(std::string(kBatteryPath) + "/capacity",
                   snapshot.capacity_percent) ||
      !readInteger(std::string(kBatteryPath) + "/voltage_now",
                   snapshot.voltage_microvolts) ||
      !readInteger(std::string(kBatteryPath) + "/current_now",
                   snapshot.current_microamps) ||
      !readInteger(std::string(kBatteryPath) + "/temp",
                   snapshot.temperature_tenths_celsius)) {
    implementation_->error =
        "read battery sysfs failed: " + std::string(std::strerror(errno));
    return false;
  }
  int usb_online = 0;
  if (!readInteger(std::string(kUsbPath) + "/online", usb_online)) {
    implementation_->error =
        "read USB power state failed: " + std::string(std::strerror(errno));
    return false;
  }
  snapshot.state = parseBatteryState(status);
  snapshot.usb_online = usb_online != 0;
  implementation_->error.clear();
  return true;
}

int PowerManager::batteryEventDescriptor() const {
  return implementation_ ? implementation_->uevent_fd : -1;
}

int PowerManager::waitForBatteryEvent(int timeout_ms,
                                      BatterySnapshot &snapshot) {
  if (!initialized()) {
    implementation_->error = "Power manager is not initialized";
    return -1;
  }
  pollfd descriptor{implementation_->uevent_fd, POLLIN, 0};
  const int ready = poll(&descriptor, 1, timeout_ms);
  if (ready < 0) {
    if (errno == EINTR)
      return 0;
    implementation_->error =
        "poll power uevent failed: " + std::string(std::strerror(errno));
    return -1;
  }
  if (ready == 0)
    return 0;
  std::array<char, 4096> message{};
  const ssize_t size =
      recv(implementation_->uevent_fd, message.data(), message.size(), 0);
  if (size < 0) {
    implementation_->error =
        "receive power uevent failed: " + std::string(std::strerror(errno));
    return -1;
  }
  bool power_supply = false;
  for (size_t offset = 0; offset < static_cast<size_t>(size);) {
    const char *field = message.data() + offset;
    const size_t length = strnlen(field, static_cast<size_t>(size) - offset);
    if (!std::strcmp(field, "SUBSYSTEM=power_supply"))
      power_supply = true;
    offset += length + 1;
  }
  if (!power_supply)
    return 0;
  return queryBattery(snapshot) ? 1 : -1;
}

bool PowerManager::setInteractive(bool interactive) {
  if (!implementation_->power || !implementation_->power->setInteractive) {
    implementation_->error = "Power HAL setInteractive is unavailable";
    return false;
  }
  implementation_->power->setInteractive(implementation_->power,
                                         interactive ? 1 : 0);
  implementation_->error.clear();
  return true;
}

bool PowerManager::acquireWakeLock(const std::string &name) {
  if (name.empty() || name.find_first_of(" \t\r\n") != std::string::npos) {
    implementation_->error = "wake lock name must be non-empty without spaces";
    return false;
  }
  if (implementation_->wake_locks.count(name) != 0)
    return true;
  if (!writeText(kWakeLockPath, name)) {
    implementation_->error =
        "acquire wake lock failed: " + std::string(std::strerror(errno));
    return false;
  }
  implementation_->wake_locks.insert(name);
  implementation_->error.clear();
  return true;
}

bool PowerManager::releaseWakeLock(const std::string &name) {
  if (implementation_->wake_locks.count(name) == 0)
    return true;
  if (!writeText(kWakeUnlockPath, name)) {
    implementation_->error =
        "release wake lock failed: " + std::string(std::strerror(errno));
    return false;
  }
  implementation_->wake_locks.erase(name);
  implementation_->error.clear();
  return true;
}

bool PowerManager::enableAutoSuspend() {
  if (implementation_->autosuspend_enabled)
    return true;
  if (autosuspend_enable() != 0) {
    implementation_->error =
        "enable autosuspend failed: " + std::string(std::strerror(errno));
    return false;
  }
  implementation_->autosuspend_enabled = true;
  implementation_->error.clear();
  return true;
}

bool PowerManager::disableAutoSuspend() {
  if (!implementation_->autosuspend_enabled)
    return true;
  if (autosuspend_disable() != 0) {
    implementation_->error =
        "disable autosuspend failed: " + std::string(std::strerror(errno));
    return false;
  }
  implementation_->autosuspend_enabled = false;
  implementation_->error.clear();
  return true;
}

bool PowerManager::scheduleRtcWake(int delay_seconds) {
  if (delay_seconds <= 0) {
    implementation_->error = "RTC wake delay must be positive";
    return false;
  }
  const int fd = open(kRtcPath, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    implementation_->error =
        "open RTC failed: " + std::string(std::strerror(errno));
    return false;
  }
  rtc_time current{};
  rtc_wkalrm alarm{};
  bool success = ioctl(fd, RTC_RD_TIME, &current) == 0;
  if (success) {
    std::time_t timestamp = timegm(reinterpret_cast<std::tm *>(&current));
    timestamp += delay_seconds;
    std::tm target{};
    success = gmtime_r(&timestamp, &target) != nullptr;
    if (success) {
      alarm.enabled = 1;
      alarm.time = *reinterpret_cast<rtc_time *>(&target);
      success = ioctl(fd, RTC_WKALM_SET, &alarm) == 0;
    }
  }
  const int saved_errno = errno;
  close(fd);
  errno = saved_errno;
  if (!success) {
    implementation_->error =
        "schedule RTC wake failed: " + std::string(std::strerror(errno));
    return false;
  }
  implementation_->error.clear();
  return true;
}

bool PowerManager::clearRtcWake() {
  const int fd = open(kRtcPath, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    implementation_->error =
        "open RTC failed: " + std::string(std::strerror(errno));
    return false;
  }
  rtc_wkalrm alarm{};
  const bool success = ioctl(fd, RTC_WKALM_SET, &alarm) == 0;
  const int saved_errno = errno;
  close(fd);
  errno = saved_errno;
  if (!success) {
    implementation_->error =
        "clear RTC wake failed: " + std::string(std::strerror(errno));
    return false;
  }
  implementation_->error.clear();
  return true;
}

bool PowerManager::suspend(int graceful_timeout_ms) {
  if (graceful_timeout_ms < 0) {
    implementation_->error = "suspend timeout cannot be negative";
    return false;
  }
  if (!writeText(kPowerStatePath, "mem")) {
    implementation_->error =
        "suspend failed: " + std::string(std::strerror(errno));
    return false;
  }
  implementation_->error.clear();
  return true;
}

FlipState PowerManager::queryFlipState(const char *input_directory) {
  DIR *directory = opendir(input_directory);
  if (!directory) {
    implementation_->error =
        "open input directory failed: " + std::string(std::strerror(errno));
    return FlipState::Unknown;
  }
  FlipState state = FlipState::Unknown;
  while (dirent *entry = readdir(directory)) {
    if (!isEventNode(entry->d_name))
      continue;
    const std::string path = std::string(input_directory) + "/" + entry->d_name;
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
      continue;
    std::array<char, 128> name{};
    if (ioctl(fd, EVIOCGNAME(name.size()), name.data()) >= 0 &&
        !std::strcmp(name.data(), "hall_sensor0")) {
      constexpr size_t kKeyWords = (KEY_MAX / kBitsPerWord) + 1;
      std::array<unsigned long, kKeyWords> keys{};
      if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys.data()) >= 0) {
        const bool pressed = (keys[kFlipKeyCode / kBitsPerWord] &
                              (1UL << (kFlipKeyCode % kBitsPerWord))) != 0;
        state = pressed ? FlipState::Closed : FlipState::Open;
      }
      close(fd);
      break;
    }
    close(fd);
  }
  closedir(directory);
  if (state == FlipState::Unknown)
    implementation_->error = "hall_sensor0 EV_KEY device was not found";
  else
    implementation_->error.clear();
  return state;
}

bool PowerManager::applyFlipKeyEvent(const oos::input::KeyEvent &event,
                                     FlipState &state) {
  if (event.code != kFlipKeyCode ||
      event.action == oos::input::KeyAction::Repeated)
    return false;
  state = event.action == oos::input::KeyAction::Pressed ? FlipState::Closed
                                                         : FlipState::Open;
  return true;
}

const std::string &PowerManager::lastError() const {
  return implementation_->error;
}

const char *batteryStateName(BatteryState state) {
  switch (state) {
  case BatteryState::Charging:
    return "charging";
  case BatteryState::Discharging:
    return "discharging";
  case BatteryState::NotCharging:
    return "not-charging";
  case BatteryState::Full:
    return "full";
  case BatteryState::Unknown:
    return "unknown";
  }
  return "unknown";
}

const char *flipStateName(FlipState state) {
  switch (state) {
  case FlipState::Open:
    return "open";
  case FlipState::Closed:
    return "closed";
  case FlipState::Unknown:
    return "unknown";
  }
  return "unknown";
}

} // namespace oos::hardware
