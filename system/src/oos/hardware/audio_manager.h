#pragma once

#include <cstdint>
#include <memory>
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

struct PcmOutputConfig {
  int sample_rate = 0;
  int channel_count = 0;
  int capacity_frames = 0;
  AudioUsage usage = AudioUsage::Media;
};

struct PcmOutputStatus {
  AudioStreamInfo stream;
  int64_t queued_frames = 0;
  int64_t consumed_frames = 0;
  int32_t underruns = 0;
  bool paused = false;
};

// A persistent, bounded signed-S16 output stream. write() is non-blocking and
// may accept fewer frames than requested; callers use status() for pacing.
class PcmOutput {
public:
  virtual ~PcmOutput() = default;
  virtual bool write(const int16_t *samples, int64_t frames,
                     int64_t &accepted_frames) = 0;
  virtual bool setVolume(float volume) = 0;
  virtual bool pause() = 0;
  virtual bool resume() = 0;
  virtual bool flush() = 0;
  // Wait until write() can accept frames. A timeout is a normal false result.
  virtual bool waitWritable(int timeout_ms) = 0;
  virtual PcmOutputStatus status() const = 0;
  virtual const std::string &lastError() const = 0;
};

class AudioManager {
public:
  bool openPcmOutput(const PcmOutputConfig &config,
                     std::unique_ptr<PcmOutput> &output, AudioStreamInfo &info);

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
