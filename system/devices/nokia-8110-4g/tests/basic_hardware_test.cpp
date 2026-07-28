#include "oos/hardware/audio_manager.h"
#include "oos/hardware/camera_manager.h"
#include "oos/hardware/power_manager.h"
#include "oos/hardware/vibrator_manager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(
        stderr,
        "usage: %s tone [ms] | record PATH [ms] | vibrate [ms] | "
        "power-status | camera-list | camera-capture PATH | torch on|off\n",
        argv[0]);
    return 2;
  }
  int duration = 1000;
  if (!std::strcmp(argv[1], "record") && argc > 3)
    duration = std::atoi(argv[3]);
  else if ((!std::strcmp(argv[1], "tone") ||
            !std::strcmp(argv[1], "vibrate")) &&
           argc > 2)
    duration = std::atoi(argv[2]);
  if ((!std::strcmp(argv[1], "record") || !std::strcmp(argv[1], "tone") ||
       !std::strcmp(argv[1], "vibrate")) &&
      (duration < 1 || duration > 10000))
    return 2;

  if (!std::strcmp(argv[1], "tone")) {
    oos::hardware::AudioManager audio;
    oos::hardware::AudioStreamInfo info;
    if (!audio.playTone(440, duration, 0.35F, oos::hardware::AudioUsage::Media,
                        info)) {
      std::fprintf(stderr, "tone failed: %s\n", audio.lastError().c_str());
      return 1;
    }
    std::printf("tone=ok rate=%d channels=%d device=%d frames=%lld\n",
                info.sample_rate, info.channel_count, info.device_id,
                static_cast<long long>(info.frames_transferred));
    return 0;
  }
  if (!std::strcmp(argv[1], "record") && argc >= 3) {
    oos::hardware::AudioManager audio;
    oos::hardware::RecordingResult result;
    if (!audio.recordWav(argv[2], duration, result)) {
      std::fprintf(stderr, "record failed: %s\n", audio.lastError().c_str());
      return 1;
    }
    std::printf("record=ok path=%s frames=%lld peak=%.6f rms=%.6f\n",
                result.path.c_str(),
                static_cast<long long>(result.stream.frames_transferred),
                result.peak, result.rms);
    return 0;
  }
  if (!std::strcmp(argv[1], "vibrate")) {
    oos::hardware::VibratorManager vibrator;
    if (!vibrator.initialize() || !vibrator.vibrate(duration)) {
      std::fprintf(stderr, "vibrate failed: %s\n",
                   vibrator.lastError().c_str());
      return 1;
    }
    usleep(static_cast<useconds_t>(duration) * 1000);
    if (!vibrator.stop())
      return 1;
    std::printf("vibrate=ok duration_ms=%d\n", duration);
    return 0;
  }
  if (!std::strcmp(argv[1], "power-status")) {
    oos::hardware::PowerManager power;
    if (!power.initialize()) {
      std::fprintf(stderr, "power init failed: %s\n",
                   power.lastError().c_str());
      return 1;
    }
    oos::hardware::BatterySnapshot battery;
    if (!power.queryBattery(battery)) {
      std::fprintf(stderr, "battery query failed: %s\n",
                   power.lastError().c_str());
      return 1;
    }
    const oos::hardware::FlipState flip = power.queryFlipState();
    if (flip == oos::hardware::FlipState::Unknown) {
      std::fprintf(stderr, "slider query failed: %s\n",
                   power.lastError().c_str());
      return 1;
    }
    if (!power.acquireWakeLock("oos-power-test") ||
        !power.releaseWakeLock("oos-power-test")) {
      std::fprintf(stderr, "wake lock cycle failed: %s\n",
                   power.lastError().c_str());
      return 1;
    }
    std::printf("power=ok battery=%s capacity=%d voltage_uv=%d "
                "current_ua=%d temperature_deci_c=%d usb_online=%d "
                "slider=%s event_fd=%d\n",
                oos::hardware::batteryStateName(battery.state),
                battery.capacity_percent, battery.voltage_microvolts,
                battery.current_microamps, battery.temperature_tenths_celsius,
                battery.usb_online, oos::hardware::flipStateName(flip),
                power.batteryEventDescriptor());
    return 0;
  }
  if (!std::strcmp(argv[1], "camera-list")) {
    oos::hardware::CameraManager camera;
    std::vector<oos::hardware::CameraInfo> cameras;
    if (!camera.initialize() || !camera.enumerate(cameras)) {
      std::fprintf(stderr, "camera list failed: %s\n",
                   camera.lastError().c_str());
      return 1;
    }
    std::printf("camera_count=%zu\n", cameras.size());
    for (const auto &info : cameras)
      std::printf("camera.%s facing=%s orientation=%d level=%s flash=%d "
                  "max_jpeg=%dx%d\n",
                  info.id.c_str(), oos::hardware::lensFacingName(info.facing),
                  info.sensor_orientation,
                  oos::hardware::cameraHardwareLevelName(info.hardware_level),
                  info.flash_available, info.max_jpeg_width,
                  info.max_jpeg_height);
    return cameras.empty() ? 1 : 0;
  }
  if (!std::strcmp(argv[1], "camera-capture") && argc >= 3) {
    oos::hardware::CameraManager camera;
    oos::hardware::PhotoResult result;
    if (!camera.initialize() ||
        !camera.captureJpeg("0", argv[2], result, 640, 480)) {
      std::fprintf(stderr, "camera capture failed: %s\n",
                   camera.lastError().c_str());
      return 1;
    }
    std::printf("camera_capture=ok path=%s size=%dx%d bytes=%zu\n",
                result.path.c_str(), result.width, result.height,
                result.byte_count);
    return 0;
  }
  if (!std::strcmp(argv[1], "torch") && argc >= 3) {
    const bool enabled = !std::strcmp(argv[2], "on");
    if (!enabled && std::strcmp(argv[2], "off"))
      return 2;
    oos::hardware::CameraManager camera;
    if (!camera.initialize() || !camera.setTorch("0", enabled)) {
      std::fprintf(stderr, "torch failed: %s\n", camera.lastError().c_str());
      return 1;
    }
    std::printf("torch=%s\n", enabled ? "on" : "off");
    return 0;
  }
  return 2;
}
