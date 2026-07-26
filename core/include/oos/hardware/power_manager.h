#pragma once

#include "oos/input/key_input.h"

#include <cstdint>
#include <memory>
#include <string>

namespace oos::hardware {

enum class BatteryState : uint8_t {
  Unknown,
  Charging,
  Discharging,
  NotCharging,
  Full,
};

enum class FlipState : uint8_t {
  Unknown,
  Open,
  Closed,
};

struct BatterySnapshot {
  BatteryState state = BatteryState::Unknown;
  int capacity_percent = -1;
  int voltage_microvolts = 0;
  int current_microamps = 0;
  int temperature_tenths_celsius = 0;
  bool usb_online = false;
};

class PowerManager {
public:
  PowerManager();
  ~PowerManager();

  PowerManager(const PowerManager &) = delete;
  PowerManager &operator=(const PowerManager &) = delete;

  bool initialize(const std::string &service_name = "default");
  void shutdown();
  bool initialized() const;

  bool queryBattery(BatterySnapshot &snapshot);

  // The uevent descriptor can be integrated into the production event loop.
  int batteryEventDescriptor() const;
  // Returns 1 after a power-supply change, 0 on timeout, and -1 on error.
  int waitForBatteryEvent(int timeout_ms, BatterySnapshot &snapshot);

  bool setInteractive(bool interactive);
  bool acquireWakeLock(const std::string &name);
  bool releaseWakeLock(const std::string &name);

  // Enables kernel-managed suspend whenever no wake source is active.
  bool enableAutoSuspend();
  bool disableAutoSuspend();

  bool scheduleRtcWake(int delay_seconds);
  bool clearRtcWake();
  // Blocks until the kernel resumes. The caller must turn displays off first.
  bool suspend(int graceful_timeout_ms = 1000);

  FlipState queryFlipState(const char *input_directory = "/dev/input");
  static bool applyFlipKeyEvent(const oos::input::KeyEvent &event,
                                FlipState &state);

  const std::string &lastError() const;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

const char *batteryStateName(BatteryState state);
const char *flipStateName(FlipState state);

} // namespace oos::hardware
