#include "oos/hardware/vibrator_manager.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace oos::hardware {
namespace {

constexpr char kVibratorEnable[] = "/sys/class/timed_output/vibrator/enable";

bool writeDuration(uint32_t duration_ms, std::string &error) {
  FILE *file = std::fopen(kVibratorEnable, "w");
  if (!file) {
    error = std::strerror(errno);
    return false;
  }
  const bool success = std::fprintf(file, "%u\n", duration_ms) > 0;
  if (!success)
    error = std::strerror(errno);
  std::fclose(file);
  return success;
}

} // namespace

struct VibratorManager::Implementation {
  bool ready = false;
  std::string error;
};

VibratorManager::VibratorManager()
    : implementation_(std::make_unique<Implementation>()) {}

VibratorManager::~VibratorManager() { shutdown(); }

bool VibratorManager::initialize(const std::string &) {
  FILE *file = std::fopen(kVibratorEnable, "r");
  implementation_->ready = file != nullptr;
  if (file)
    std::fclose(file);
  if (!implementation_->ready)
    implementation_->error = std::strerror(errno);
  else
    implementation_->error.clear();
  return implementation_->ready;
}

void VibratorManager::shutdown() {
  if (implementation_ && implementation_->ready)
    writeDuration(0, implementation_->error);
  if (implementation_)
    implementation_->ready = false;
}

bool VibratorManager::initialized() const { return implementation_->ready; }

bool VibratorManager::vibrate(uint32_t duration_ms) {
  if (!initialized() || duration_ms == 0) {
    implementation_->error = "invalid vibration request";
    return false;
  }
  return writeDuration(duration_ms, implementation_->error);
}

bool VibratorManager::stop() {
  if (!initialized()) {
    implementation_->error = "vibrator is not initialized";
    return false;
  }
  return writeDuration(0, implementation_->error);
}

bool VibratorManager::supportsAmplitudeControl() { return false; }

bool VibratorManager::setAmplitude(uint8_t) {
  implementation_->error = "vibrator amplitude control is unsupported";
  return false;
}

const std::string &VibratorManager::lastError() const {
  return implementation_->error;
}

} // namespace oos::hardware
