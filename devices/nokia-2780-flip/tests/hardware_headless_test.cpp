#include "oos/hardware/audio_manager.h"
#include "oos/hardware/camera_manager.h"
#include "oos/hardware/codec_manager.h"
#include "oos/hardware/power_manager.h"
#include "oos/hardware/vibrator_manager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace {

void usage(const char *program) {
  std::fprintf(stderr,
               "Usage:\n"
               "  %s audio-tone [duration-ms] [frequency-hz]\n"
               "  %s audio-record PATH [duration-ms]\n"
               "  %s vibrate [duration-ms] [amplitude]\n"
               "  %s power-status\n"
               "  %s power-cycle [duration-ms]\n"
               "  %s suspend-test [wake-delay-seconds]\n"
               "  %s camera-list\n"
               "  %s camera-capture CAMERA-ID PATH [flash]\n"
               "  %s torch CAMERA-ID [duration-ms]\n"
               "  %s codec-h264 [frame-count]\n",
               program, program, program, program, program, program, program,
               program, program, program);
}

int parsePositive(const char *value, int fallback) {
  if (!value)
    return fallback;
  const long parsed = std::strtol(value, nullptr, 10);
  return parsed > 0 && parsed <= 60000 ? static_cast<int>(parsed) : -1;
}

int runTone(int argc, char **argv) {
  const int duration = parsePositive(argc > 2 ? argv[2] : nullptr, 1500);
  const int frequency = parsePositive(argc > 3 ? argv[3] : nullptr, 440);
  if (duration < 0 || frequency < 0)
    return 2;
  oos::hardware::AudioManager audio;
  oos::hardware::AudioStreamInfo info;
  if (!audio.playTone(frequency, duration, 0.35F,
                      oos::hardware::AudioUsage::Media, info)) {
    std::fprintf(stderr, "Tone failed: %s\n", audio.lastError().c_str());
    return 1;
  }
  std::printf(
      "audio_tone=ok sample_rate=%d channels=%d device=%d frames=%lld\n",
      info.sample_rate, info.channel_count, info.device_id,
      static_cast<long long>(info.frames_transferred));
  return 0;
}

int runRecord(int argc, char **argv) {
  if (argc < 3)
    return 2;
  const int duration = parsePositive(argc > 3 ? argv[3] : nullptr, 2000);
  if (duration < 0)
    return 2;
  oos::hardware::AudioManager audio;
  oos::hardware::RecordingResult result;
  if (!audio.recordWav(argv[2], duration, result)) {
    std::fprintf(stderr, "Recording failed: %s\n", audio.lastError().c_str());
    return 1;
  }
  std::printf("audio_record=ok path=%s sample_rate=%d channels=%d device=%d "
              "frames=%lld peak=%.6f rms=%.6f\n",
              result.path.c_str(), result.stream.sample_rate,
              result.stream.channel_count, result.stream.device_id,
              static_cast<long long>(result.stream.frames_transferred),
              result.peak, result.rms);
  return 0;
}

int runVibrate(int argc, char **argv) {
  const int duration = parsePositive(argc > 2 ? argv[2] : nullptr, 500);
  const int amplitude = parsePositive(argc > 3 ? argv[3] : nullptr, 128);
  if (duration < 0 || amplitude < 1 || amplitude > 255)
    return 2;
  oos::hardware::VibratorManager vibrator;
  if (!vibrator.initialize()) {
    std::fprintf(stderr, "Vibrator initialization failed: %s\n",
                 vibrator.lastError().c_str());
    return 1;
  }
  const bool amplitude_supported = vibrator.supportsAmplitudeControl();
  if (amplitude_supported && !vibrator.setAmplitude(amplitude)) {
    std::fprintf(stderr, "Vibrator amplitude failed: %s\n",
                 vibrator.lastError().c_str());
    return 1;
  }
  if (!vibrator.vibrate(duration)) {
    std::fprintf(stderr, "Vibration failed: %s\n",
                 vibrator.lastError().c_str());
    return 1;
  }
  usleep(static_cast<useconds_t>(duration) * 1000U);
  if (!vibrator.stop()) {
    std::fprintf(stderr, "Vibrator cleanup failed: %s\n",
                 vibrator.lastError().c_str());
    return 1;
  }
  std::printf("vibrator=ok duration_ms=%d amplitude_control=%d\n", duration,
              amplitude_supported);
  return 0;
}

int runPowerStatus() {
  oos::hardware::PowerManager power;
  if (!power.initialize()) {
    std::fprintf(stderr, "Power initialization failed: %s\n",
                 power.lastError().c_str());
    return 1;
  }
  oos::hardware::BatterySnapshot battery;
  if (!power.queryBattery(battery)) {
    std::fprintf(stderr, "Battery query failed: %s\n",
                 power.lastError().c_str());
    return 1;
  }
  const oos::hardware::FlipState flip = power.queryFlipState();
  if (flip == oos::hardware::FlipState::Unknown) {
    std::fprintf(stderr, "Flip query failed: %s\n", power.lastError().c_str());
    return 1;
  }
  std::printf("battery_state=%s capacity=%d voltage_uv=%d current_ua=%d "
              "temperature_deci_c=%d usb_online=%d\n",
              oos::hardware::batteryStateName(battery.state),
              battery.capacity_percent, battery.voltage_microvolts,
              battery.current_microamps, battery.temperature_tenths_celsius,
              battery.usb_online);
  std::printf("flip_state=%s battery_event_fd=%d\n",
              oos::hardware::flipStateName(flip),
              power.batteryEventDescriptor());
  return 0;
}

int runPowerCycle(int argc, char **argv) {
  const int duration = parsePositive(argc > 2 ? argv[2] : nullptr, 1000);
  if (duration < 0)
    return 2;
  oos::hardware::PowerManager power;
  if (!power.initialize() || !power.acquireWakeLock("oos-hardware-test")) {
    std::fprintf(stderr, "Power cycle setup failed: %s\n",
                 power.lastError().c_str());
    return 1;
  }
  if (!power.setInteractive(false)) {
    std::fprintf(stderr, "Set non-interactive failed: %s\n",
                 power.lastError().c_str());
    return 1;
  }
  usleep(static_cast<useconds_t>(duration) * 1000U);
  if (!power.setInteractive(true) ||
      !power.releaseWakeLock("oos-hardware-test")) {
    std::fprintf(stderr, "Restore interactive state failed: %s\n",
                 power.lastError().c_str());
    return 1;
  }
  std::printf("power_cycle=ok duration_ms=%d\n", duration);
  return 0;
}

int runSuspendTest(int argc, char **argv) {
  const int delay = parsePositive(argc > 2 ? argv[2] : nullptr, 5);
  if (delay < 0)
    return 2;
  oos::hardware::PowerManager power;
  if (!power.initialize() || !power.scheduleRtcWake(delay) ||
      !power.setInteractive(false)) {
    std::fprintf(stderr, "Suspend setup failed: %s\n",
                 power.lastError().c_str());
    return 1;
  }
  const bool suspended = power.suspend();
  const std::string suspend_error = power.lastError();
  const bool interactive = power.setInteractive(true);
  const std::string interactive_error = power.lastError();
  const bool alarm_cleared = power.clearRtcWake();
  const std::string alarm_error = power.lastError();
  if (!suspended || !interactive || !alarm_cleared) {
    std::fprintf(stderr,
                 "Suspend lifecycle failed: suspend=%d (%s) interactive=%d "
                 "(%s) alarm_clear=%d (%s)\n",
                 suspended, suspend_error.c_str(), interactive,
                 interactive_error.c_str(), alarm_cleared, alarm_error.c_str());
    return 1;
  }
  std::printf("suspend_test=ok wake_delay_seconds=%d\n", delay);
  return 0;
}

bool initializeCamera(oos::hardware::CameraManager &camera) {
  if (camera.initialize())
    return true;
  std::fprintf(stderr, "Camera initialization failed: %s\n",
               camera.lastError().c_str());
  return false;
}

int runCameraList() {
  oos::hardware::CameraManager camera;
  if (!initializeCamera(camera))
    return 1;
  std::vector<oos::hardware::CameraInfo> cameras;
  if (!camera.enumerate(cameras)) {
    std::fprintf(stderr, "Camera enumeration failed: %s\n",
                 camera.lastError().c_str());
    return 1;
  }
  std::printf("camera_count=%zu\n", cameras.size());
  for (const auto &info : cameras) {
    std::printf("camera.%s facing=%s orientation=%d hardware=%s(%d) "
                "flash=%d max_jpeg=%dx%d\n",
                info.id.c_str(), oos::hardware::lensFacingName(info.facing),
                info.sensor_orientation,
                oos::hardware::cameraHardwareLevelName(info.hardware_level),
                info.hardware_level, info.flash_available, info.max_jpeg_width,
                info.max_jpeg_height);
  }
  return cameras.empty() ? 1 : 0;
}

int runCameraCapture(int argc, char **argv) {
  if (argc < 4)
    return 2;
  const bool flash = argc > 4 && !std::strcmp(argv[4], "flash");
  if (argc > 4 && !flash)
    return 2;
  oos::hardware::CameraManager camera;
  if (!initializeCamera(camera))
    return 1;
  oos::hardware::PhotoResult result;
  if (!camera.captureJpeg(argv[2], argv[3], result, 1920, 1080, flash)) {
    std::fprintf(stderr, "Camera capture failed: %s\n",
                 camera.lastError().c_str());
    return 1;
  }
  std::printf("camera_capture=ok camera=%s path=%s size=%dx%d bytes=%zu "
              "flash=%d\n",
              argv[2], result.path.c_str(), result.width, result.height,
              result.byte_count, flash);
  return 0;
}

int runTorch(int argc, char **argv) {
  if (argc < 3)
    return 2;
  const int duration = parsePositive(argc > 3 ? argv[3] : nullptr, 1000);
  if (duration < 0)
    return 2;
  oos::hardware::CameraManager camera;
  if (!initializeCamera(camera))
    return 1;
  if (!camera.setTorch(argv[2], true)) {
    std::fprintf(stderr, "Torch on failed: %s\n", camera.lastError().c_str());
    return 1;
  }
  usleep(static_cast<useconds_t>(duration) * 1000U);
  if (!camera.setTorch(argv[2], false)) {
    std::fprintf(stderr, "Torch off failed: %s\n", camera.lastError().c_str());
    return 1;
  }
  std::printf("torch=ok camera=%s duration_ms=%d\n", argv[2], duration);
  return 0;
}

int runCodecH264(int argc, char **argv) {
  const int frame_count = parsePositive(argc > 2 ? argv[2] : nullptr, 30);
  if (frame_count < 0)
    return 2;
  oos::hardware::CodecManager codec;
  oos::hardware::CodecResult result;
  if (!codec.testH264RoundTrip(320, 240, frame_count, result)) {
    std::fprintf(stderr, "H.264 codec test failed: %s\n",
                 codec.lastError().c_str());
    return 1;
  }
  std::printf("codec_h264=ok encoder=%s encoder_hardware=%d decoder=%s "
              "decoder_hardware=%d size=%dx%d input_frames=%d "
              "output_buffers=%d decoded_frames=%d bytes=%zu\n",
              result.encoder_name.c_str(), result.encoder_hardware_accelerated,
              result.decoder_name.c_str(), result.decoder_hardware_accelerated,
              result.width, result.height, result.input_frames,
              result.output_buffers, result.decoded_frames,
              result.encoded_bytes);
  return result.encoder_hardware_accelerated &&
                 result.decoder_hardware_accelerated
             ? 0
             : 1;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 2;
  }
  if (!std::strcmp(argv[1], "audio-tone"))
    return runTone(argc, argv);
  if (!std::strcmp(argv[1], "audio-record"))
    return runRecord(argc, argv);
  if (!std::strcmp(argv[1], "vibrate"))
    return runVibrate(argc, argv);
  if (!std::strcmp(argv[1], "power-status"))
    return runPowerStatus();
  if (!std::strcmp(argv[1], "power-cycle"))
    return runPowerCycle(argc, argv);
  if (!std::strcmp(argv[1], "suspend-test"))
    return runSuspendTest(argc, argv);
  if (!std::strcmp(argv[1], "camera-list"))
    return runCameraList();
  if (!std::strcmp(argv[1], "camera-capture"))
    return runCameraCapture(argc, argv);
  if (!std::strcmp(argv[1], "torch"))
    return runTorch(argc, argv);
  if (!std::strcmp(argv[1], "codec-h264"))
    return runCodecH264(argc, argv);
  usage(argv[0]);
  return 2;
}
