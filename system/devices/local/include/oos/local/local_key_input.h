#pragma once

#include "oos/input/key_input.h"

#include <cstdint>
#include <memory>

namespace oos::local {

class LocalKeyInput final : public input::KeyInputSource {
public:
  LocalKeyInput();
  ~LocalKeyInput() override;

  LocalKeyInput(const LocalKeyInput &) = delete;
  LocalKeyInput &operator=(const LocalKeyInput &) = delete;

  bool initialize(const char *mapping_path);
  void shutdown() override;
  int poll(int timeout_ms, input::KeyEventCallback callback,
           void *context) override;
  bool initialized() const override;
  int fileDescriptor() const override;
  const std::vector<input::KeyDeviceInfo> &devices() const override;
  bool stopRequested() const override;
  const char *lastError() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::local
