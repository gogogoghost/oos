#include "oos/hardware/vibrator_manager.h"

#include <android/hardware/vibrator/1.0/IVibrator.h>

namespace oos::hardware {

namespace vibrator = ::android::hardware::vibrator::V1_0;
using ::android::sp;

struct VibratorManager::Implementation {
  std::string error;
  sp<vibrator::IVibrator> service;
};

VibratorManager::VibratorManager()
    : implementation_(std::make_unique<Implementation>()) {}

VibratorManager::~VibratorManager() { shutdown(); }

bool VibratorManager::initialize(const std::string &service_name) {
  shutdown();
  implementation_->service = vibrator::IVibrator::getService(service_name);
  if (!implementation_->service) {
    implementation_->error = "Vibrator HAL service not found: " + service_name;
    return false;
  }
  implementation_->error.clear();
  return true;
}

void VibratorManager::shutdown() {
  if (!implementation_)
    return;
  if (implementation_->service)
    stop();
  implementation_->service.clear();
}

bool VibratorManager::initialized() const {
  return implementation_->service != nullptr;
}

bool VibratorManager::vibrate(uint32_t duration_ms) {
  if (!initialized() || duration_ms == 0) {
    implementation_->error =
        "Vibrator HAL is not initialized or duration is zero";
    return false;
  }
  const auto result = implementation_->service->on(duration_ms);
  if (!result.isOk()) {
    implementation_->error =
        "vibrator on transport failed: " + result.description();
    return false;
  }
  if (static_cast<vibrator::Status>(result) != vibrator::Status::OK) {
    implementation_->error = "vibrator rejected on request";
    return false;
  }
  implementation_->error.clear();
  return true;
}

bool VibratorManager::stop() {
  if (!initialized()) {
    implementation_->error = "Vibrator HAL is not initialized";
    return false;
  }
  const auto result = implementation_->service->off();
  if (!result.isOk()) {
    implementation_->error =
        "vibrator off transport failed: " + result.description();
    return false;
  }
  if (static_cast<vibrator::Status>(result) != vibrator::Status::OK) {
    implementation_->error = "vibrator rejected off request";
    return false;
  }
  implementation_->error.clear();
  return true;
}

bool VibratorManager::supportsAmplitudeControl() {
  if (!initialized())
    return false;
  const auto result = implementation_->service->supportsAmplitudeControl();
  return result.isOk() && static_cast<bool>(result);
}

bool VibratorManager::setAmplitude(uint8_t amplitude) {
  if (!initialized() || amplitude == 0) {
    implementation_->error =
        "Vibrator HAL is not initialized or amplitude is zero";
    return false;
  }
  const auto result = implementation_->service->setAmplitude(amplitude);
  if (!result.isOk()) {
    implementation_->error =
        "set vibrator amplitude transport failed: " + result.description();
    return false;
  }
  if (static_cast<vibrator::Status>(result) != vibrator::Status::OK) {
    implementation_->error = "vibrator amplitude control is unsupported";
    return false;
  }
  implementation_->error.clear();
  return true;
}

const std::string &VibratorManager::lastError() const {
  return implementation_->error;
}

} // namespace oos::hardware
