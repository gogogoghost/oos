#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace oos::input {

enum class KeyAction : uint8_t {
  Released = 0,
  Pressed = 1,
  Repeated = 2,
};

struct KeyEvent {
  int64_t timestamp_us = 0;
  uint16_t code = 0;
  KeyAction action = KeyAction::Released;
  // Views remain valid for the duration of the synchronous callback. This
  // keeps the input hot path allocation-free for evdev and local backends.
  std::string_view device_path;
  std::string_view device_name;
};

struct KeyDeviceInfo {
  std::string path;
  std::string name;
};

struct KeyInputOptions {
  // EVIOCGRAB prevents another userspace process from receiving these keys.
  // Production OOS can enable it after taking over from B2G; diagnostics
  // should normally leave it disabled.
  bool grab_devices = false;
};

using KeyEventCallback = void (*)(void *context, const KeyEvent &event);

// Device input sources expose one allocation-free polling contract. Concrete
// backends are selected by the device build and remain statically owned there.
class KeyInputSource {
public:
  virtual ~KeyInputSource() = default;

  virtual void shutdown() = 0;
  virtual int poll(int timeout_ms, KeyEventCallback callback,
                   void *context) = 0;
  virtual bool initialized() const = 0;
  virtual int fileDescriptor() const = 0;
  virtual const std::vector<KeyDeviceInfo> &devices() const = 0;
  virtual bool stopRequested() const { return false; }
};

// Discovers Linux evdev nodes with EV_KEY capability and multiplexes them
// through one epoll descriptor. The object is not thread-safe; initialize,
// poll, and shutdown it from the same input thread or event loop.
class KeyInput final : public KeyInputSource {
public:
  explicit KeyInput(KeyInputOptions options = {});
  ~KeyInput();

  KeyInput(const KeyInput &) = delete;
  KeyInput &operator=(const KeyInput &) = delete;
  KeyInput(KeyInput &&) = delete;
  KeyInput &operator=(KeyInput &&) = delete;

  bool initialize(const char *input_directory = "/dev/input");
  void shutdown() override;

  // Waits for up to timeout_ms and dispatches every complete EV_KEY event.
  // Returns the number of dispatched events, or -1 on an epoll/read failure.
  int poll(int timeout_ms, KeyEventCallback callback, void *context) override;

  bool initialized() const override;
  // The epoll descriptor is itself pollable and can be registered with a
  // native event loop. Call poll(0, ...) when it becomes readable.
  int fileDescriptor() const override;
  const std::vector<KeyDeviceInfo> &devices() const override;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

const char *keyActionName(KeyAction action);
const char *keyCodeName(uint16_t code);

} // namespace oos::input
