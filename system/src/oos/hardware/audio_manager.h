#pragma once

#include <cstdint>
#include <string>

namespace oos::hardware {

enum class AudioUsage : uint8_t {
  Media,
  Voice,
  Ringtone,
  Alarm,
  Notification,
};

struct AudioStreamInfo {
  int sample_rate = 0;
  int channel_count = 0;
  int device_id = 0;
  int64_t frames_transferred = 0;
};

struct RecordingResult {
  AudioStreamInfo stream;
  double peak = 0.0;
  double rms = 0.0;
  std::string path;
};

class AudioManager {
public:
  // Synchronously renders a sine tone through Android's audio policy service.
  bool playTone(double frequency_hz, int duration_ms, float volume,
                AudioUsage usage, AudioStreamInfo &info);

  // Synchronously records signed 16-bit PCM and writes a standard WAV file.
  bool recordWav(const std::string &path, int duration_ms,
                 RecordingResult &result);

  const std::string &lastError() const;

private:
  std::string error_;
};

} // namespace oos::hardware
