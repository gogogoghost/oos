#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "oos/input/key_input.h"

namespace oos::device {
class Device;
}

namespace oos::runtime {

class GraphicsHost;

struct NativeAppLaunchOptions {
  const char *module_path = nullptr;
  const char *data_directory = nullptr;
  const char *internal_media_directory = "/data/media/internal";
  const char *removable_media_directory = "/data/media/removable";
  uint32_t stack_size = 128 * 1024;
  uint32_t heap_size = 4 * 1024 * 1024;
  uint32_t service_permission_mask = 0;
  bool enforce_service_permissions = false;
};

class NativeAppManager {
public:
  static constexpr size_t kDefaultResidentLimit = 3;

  explicit NativeAppManager(
      GraphicsHost &graphics,
      size_t resident_limit = NativeAppManager::kDefaultResidentLimit);
  NativeAppManager(
      GraphicsHost &graphics, device::Device &device,
      size_t resident_limit = NativeAppManager::kDefaultResidentLimit);
  ~NativeAppManager();

  NativeAppManager(const NativeAppManager &) = delete;
  NativeAppManager &operator=(const NativeAppManager &) = delete;

  bool load(const char *id, const char *module_path);
  bool load(const char *id, const NativeAppLaunchOptions &options);
  bool activate(const char *id);
  bool remove(const char *id);
  bool dispatchKey(const input::KeyEvent &event, int64_t monotonic_us);
  bool render(int64_t monotonic_us);
  void shutdown();

  size_t residentCount() const;
  const char *activeId() const;
  const char *lastError() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::runtime
