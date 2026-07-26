#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace oos::hardware {

class VibratorManager {
public:
  VibratorManager();
  ~VibratorManager();

  VibratorManager(const VibratorManager &) = delete;
  VibratorManager &operator=(const VibratorManager &) = delete;

  bool initialize(const std::string &service_name = "default");
  void shutdown();
  bool initialized() const;

  bool vibrate(uint32_t duration_ms);
  bool stop();
  bool supportsAmplitudeControl();
  bool setAmplitude(uint8_t amplitude);

  const std::string &lastError() const;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace oos::hardware
