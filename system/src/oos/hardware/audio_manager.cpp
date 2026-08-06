#include "oos/hardware/audio_manager.h"

#include <aaudio/AAudio.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace oos::hardware {
namespace {

constexpr int kRequestedSampleRate = 48000;
constexpr int kRequestedChannels = 1;
constexpr int64_t kIoTimeoutNanoseconds = 2'000'000'000LL;
constexpr double kPi = 3.14159265358979323846;

aaudio_usage_t toAAudioUsage(AudioUsage usage) {
  switch (usage) {
  case AudioUsage::Media:
    return AAUDIO_USAGE_MEDIA;
  case AudioUsage::Voice:
    return AAUDIO_USAGE_VOICE_COMMUNICATION;
  case AudioUsage::Ringtone:
    return AAUDIO_USAGE_NOTIFICATION_RINGTONE;
  case AudioUsage::Alarm:
    return AAUDIO_USAGE_ALARM;
  case AudioUsage::Notification:
    return AAUDIO_USAGE_NOTIFICATION;
  }
  return AAUDIO_USAGE_MEDIA;
}

std::string aaudioError(const char *operation, aaudio_result_t result) {
  return std::string(operation) + ": " + AAudio_convertResultToText(result) +
         " (" + std::to_string(result) + ")";
}

void writeLittleEndian16(std::FILE *file, uint16_t value) {
  const std::array<uint8_t, 2> bytes = {static_cast<uint8_t>(value),
                                        static_cast<uint8_t>(value >> 8)};
  std::fwrite(bytes.data(), 1, bytes.size(), file);
}

void writeLittleEndian32(std::FILE *file, uint32_t value) {
  const std::array<uint8_t, 4> bytes = {
      static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
      static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  std::fwrite(bytes.data(), 1, bytes.size(), file);
}

bool writeWavHeader(std::FILE *file, int sample_rate, int channels,
                    uint32_t data_size) {
  if (std::fseek(file, 0, SEEK_SET) != 0)
    return false;
  std::fwrite("RIFF", 1, 4, file);
  writeLittleEndian32(file, 36 + data_size);
  std::fwrite("WAVEfmt ", 1, 8, file);
  writeLittleEndian32(file, 16);
  writeLittleEndian16(file, 1);
  writeLittleEndian16(file, static_cast<uint16_t>(channels));
  writeLittleEndian32(file, static_cast<uint32_t>(sample_rate));
  writeLittleEndian32(file, static_cast<uint32_t>(sample_rate * channels * 2));
  writeLittleEndian16(file, static_cast<uint16_t>(channels * 2));
  writeLittleEndian16(file, 16);
  std::fwrite("data", 1, 4, file);
  writeLittleEndian32(file, data_size);
  return !std::ferror(file);
}

class StreamGuard {
public:
  ~StreamGuard() {
    if (stream)
      AAudioStream_close(stream);
    if (builder)
      AAudioStreamBuilder_delete(builder);
  }

  AAudioStreamBuilder *builder = nullptr;
  AAudioStream *stream = nullptr;
};

class AAudioPcmOutput final : public PcmOutput {
public:
  ~AAudioPcmOutput() override {
    if (stream_) {
      AAudioStream_requestStop(stream_);
      AAudioStream_close(stream_);
    }
  }

  bool open(const PcmOutputConfig &config, AudioStreamInfo &info) {
    if (config.sample_rate < 8000 || config.sample_rate > 192000 ||
        config.channel_count < 1 || config.channel_count > 2 ||
        config.capacity_frames < 256 || config.capacity_frames > 65536) {
      error_ = "invalid PCM output configuration";
      return false;
    }
    AAudioStreamBuilder *builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
      error_ = aaudioError("create PCM builder failed", result);
      return false;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder,
                                           AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(builder, config.sample_rate);
    AAudioStreamBuilder_setChannelCount(builder, config.channel_count);
    AAudioStreamBuilder_setUsage(builder, toAAudioUsage(config.usage));
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
    result = AAudioStreamBuilder_openStream(builder, &stream_);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK) {
      error_ = aaudioError("open PCM stream failed", result);
      stream_ = nullptr;
      return false;
    }
    capacity_frames_ = config.capacity_frames;
    AAudioStream_setBufferSizeInFrames(stream_, capacity_frames_);
    result = AAudioStream_requestStart(stream_);
    if (result != AAUDIO_OK) {
      error_ = aaudioError("start PCM stream failed", result);
      return false;
    }
    info = {AAudioStream_getSampleRate(stream_),
            AAudioStream_getChannelCount(stream_),
            AAudioStream_getDeviceId(stream_), 0};
    channels_ = info.channel_count;
    return true;
  }

  bool write(const int16_t *samples, int64_t frames,
             int64_t &accepted_frames) override {
    accepted_frames = 0;
    if (!stream_ || !samples || frames <= 0) {
      error_ = "invalid PCM write";
      return false;
    }
    const int64_t queued =
        std::max<int64_t>(0, AAudioStream_getFramesWritten(stream_) -
                                 AAudioStream_getFramesRead(stream_));
    const int64_t writable = std::min<int64_t>(
        frames, std::max<int64_t>(0, capacity_frames_ - queued));
    if (writable == 0)
      return true;
    const int16_t *source = samples;
    if (volume_ != 1.0F) {
      scaled_.resize(static_cast<size_t>(writable) * channels_);
      for (size_t index = 0; index < scaled_.size(); ++index) {
        const float value = static_cast<float>(samples[index]) * volume_;
        scaled_[index] = static_cast<int16_t>(
            std::max(-32768.0F, std::min(32767.0F, value)));
      }
      source = scaled_.data();
    }
    const aaudio_result_t written =
        AAudioStream_write(stream_, source, writable, 0);
    if (written < 0) {
      error_ = aaudioError("write PCM stream failed", written);
      return false;
    }
    accepted_frames = written;
    return true;
  }

  bool setVolume(float volume) override {
    if (volume < 0.0F || volume > 1.0F) {
      error_ = "PCM volume is outside 0..1";
      return false;
    }
    volume_ = volume;
    return true;
  }

  bool pause() override {
    const aaudio_result_t result = AAudioStream_requestPause(stream_);
    if (result != AAUDIO_OK) {
      error_ = aaudioError("pause PCM stream failed", result);
      return false;
    }
    paused_ = true;
    return true;
  }

  bool resume() override {
    const aaudio_result_t result = AAudioStream_requestStart(stream_);
    if (result != AAUDIO_OK) {
      error_ = aaudioError("resume PCM stream failed", result);
      return false;
    }
    paused_ = false;
    return true;
  }

  bool flush() override {
    const bool restart = !paused_;
    aaudio_result_t result = AAudioStream_requestPause(stream_);
    if (result == AAUDIO_OK)
      result = AAudioStream_requestFlush(stream_);
    if (result == AAUDIO_OK && restart)
      result = AAudioStream_requestStart(stream_);
    if (result != AAUDIO_OK) {
      error_ = aaudioError("flush PCM stream failed", result);
      return false;
    }
    return true;
  }

  bool waitWritable(int timeout_ms) override {
    if (!stream_ || timeout_ms <= 0 || paused_)
      return false;
    const int64_t queued =
        std::max<int64_t>(0, AAudioStream_getFramesWritten(stream_) -
                                 AAudioStream_getFramesRead(stream_));
    if (queued < capacity_frames_)
      return true;
    const int32_t sample_rate = AAudioStream_getSampleRate(stream_);
    const int64_t wait_us =
        sample_rate > 0 ? std::max<int64_t>(1000, 1000000 / sample_rate) : 1000;
    std::this_thread::sleep_for(std::chrono::microseconds(
        std::min<int64_t>(wait_us, static_cast<int64_t>(timeout_ms) * 1000)));
    return AAudioStream_getFramesWritten(stream_) -
               AAudioStream_getFramesRead(stream_) <
           capacity_frames_;
  }

  PcmOutputStatus status() const override {
    if (!stream_)
      return {};
    const int64_t written = AAudioStream_getFramesWritten(stream_);
    const int64_t read = AAudioStream_getFramesRead(stream_);
    return {{AAudioStream_getSampleRate(stream_),
             AAudioStream_getChannelCount(stream_),
             AAudioStream_getDeviceId(stream_), written},
            std::max<int64_t>(0, written - read),
            read,
            AAudioStream_getXRunCount(stream_),
            paused_};
  }

  const std::string &lastError() const override { return error_; }

private:
  AAudioStream *stream_ = nullptr;
  int32_t capacity_frames_ = 0;
  int32_t channels_ = 0;
  float volume_ = 1.0F;
  bool paused_ = false;
  std::vector<int16_t> scaled_;
  std::string error_;
};

} // namespace

bool AudioManager::openPcmOutput(const PcmOutputConfig &config,
                                 std::unique_ptr<PcmOutput> &output,
                                 AudioStreamInfo &info) {
  output.reset();
  error_.clear();
  auto candidate = std::make_unique<AAudioPcmOutput>();
  if (!candidate->open(config, info)) {
    error_ = candidate->lastError();
    return false;
  }
  output = std::move(candidate);
  return true;
}

bool AudioManager::playTone(double frequency_hz, int duration_ms, float volume,
                            AudioUsage usage, AudioStreamInfo &info) {
  info = {};
  error_.clear();
  if (frequency_hz <= 0.0 || duration_ms <= 0 || volume < 0.0F ||
      volume > 1.0F) {
    error_ = "invalid tone parameters";
    return false;
  }

  StreamGuard guard;
  aaudio_result_t status = AAudio_createStreamBuilder(&guard.builder);
  if (status != AAUDIO_OK) {
    error_ = aaudioError("create output builder failed", status);
    return false;
  }
  AAudioStreamBuilder_setDirection(guard.builder, AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setSharingMode(guard.builder, AAUDIO_SHARING_MODE_SHARED);
  AAudioStreamBuilder_setPerformanceMode(guard.builder,
                                         AAUDIO_PERFORMANCE_MODE_NONE);
  AAudioStreamBuilder_setFormat(guard.builder, AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setSampleRate(guard.builder, kRequestedSampleRate);
  AAudioStreamBuilder_setChannelCount(guard.builder, kRequestedChannels);
  AAudioStreamBuilder_setUsage(guard.builder, toAAudioUsage(usage));
  AAudioStreamBuilder_setContentType(guard.builder,
                                     AAUDIO_CONTENT_TYPE_SONIFICATION);
  status = AAudioStreamBuilder_openStream(guard.builder, &guard.stream);
  if (status != AAUDIO_OK) {
    error_ = aaudioError("open output stream failed", status);
    return false;
  }

  info.sample_rate = AAudioStream_getSampleRate(guard.stream);
  info.channel_count = AAudioStream_getChannelCount(guard.stream);
  info.device_id = AAudioStream_getDeviceId(guard.stream);
  if (info.sample_rate <= 0 || info.channel_count <= 0) {
    error_ = "output stream returned invalid format";
    return false;
  }
  status = AAudioStream_requestStart(guard.stream);
  if (status != AAUDIO_OK) {
    error_ = aaudioError("start output stream failed", status);
    return false;
  }

  constexpr int kChunkFrames = 256;
  std::vector<int16_t> samples(kChunkFrames * info.channel_count);
  const int64_t target_frames =
      static_cast<int64_t>(info.sample_rate) * duration_ms / 1000;
  double phase = 0.0;
  const double phase_step = 2.0 * kPi * frequency_hz / info.sample_rate;
  const double amplitude = std::numeric_limits<int16_t>::max() * volume;
  while (info.frames_transferred < target_frames) {
    const int frames = static_cast<int>(std::min<int64_t>(
        kChunkFrames, target_frames - info.frames_transferred));
    for (int frame = 0; frame < frames; ++frame) {
      const int16_t sample = static_cast<int16_t>(std::sin(phase) * amplitude);
      phase += phase_step;
      if (phase >= 2.0 * kPi)
        phase -= 2.0 * kPi;
      for (int channel = 0; channel < info.channel_count; ++channel)
        samples[frame * info.channel_count + channel] = sample;
    }
    const aaudio_result_t written = AAudioStream_write(
        guard.stream, samples.data(), frames, kIoTimeoutNanoseconds);
    if (written < 0) {
      error_ = aaudioError("write output stream failed", written);
      AAudioStream_requestStop(guard.stream);
      return false;
    }
    if (written == 0) {
      error_ = "write output stream made no progress";
      AAudioStream_requestStop(guard.stream);
      return false;
    }
    info.frames_transferred += written;
  }
  AAudioStream_requestStop(guard.stream);
  return true;
}

bool AudioManager::recordWav(const std::string &path, int duration_ms,
                             RecordingResult &result) {
  result = {};
  error_.clear();
  if (path.empty() || duration_ms <= 0) {
    error_ = "invalid recording parameters";
    return false;
  }

  StreamGuard guard;
  aaudio_result_t status = AAudio_createStreamBuilder(&guard.builder);
  if (status != AAUDIO_OK) {
    error_ = aaudioError("create input builder failed", status);
    return false;
  }
  AAudioStreamBuilder_setDirection(guard.builder, AAUDIO_DIRECTION_INPUT);
  AAudioStreamBuilder_setSharingMode(guard.builder, AAUDIO_SHARING_MODE_SHARED);
  AAudioStreamBuilder_setPerformanceMode(guard.builder,
                                         AAUDIO_PERFORMANCE_MODE_NONE);
  AAudioStreamBuilder_setFormat(guard.builder, AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setSampleRate(guard.builder, kRequestedSampleRate);
  AAudioStreamBuilder_setChannelCount(guard.builder, kRequestedChannels);
  AAudioStreamBuilder_setInputPreset(guard.builder,
                                     AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
  status = AAudioStreamBuilder_openStream(guard.builder, &guard.stream);
  if (status != AAUDIO_OK) {
    error_ = aaudioError("open input stream failed", status);
    return false;
  }

  result.stream.sample_rate = AAudioStream_getSampleRate(guard.stream);
  result.stream.channel_count = AAudioStream_getChannelCount(guard.stream);
  result.stream.device_id = AAudioStream_getDeviceId(guard.stream);
  result.path = path;
  if (result.stream.sample_rate <= 0 || result.stream.channel_count <= 0) {
    error_ = "input stream returned invalid format";
    return false;
  }

  std::FILE *file = std::fopen(path.c_str(), "wb");
  if (!file) {
    error_ = "open WAV output failed";
    return false;
  }
  writeWavHeader(file, result.stream.sample_rate, result.stream.channel_count,
                 0);
  status = AAudioStream_requestStart(guard.stream);
  if (status != AAUDIO_OK) {
    error_ = aaudioError("start input stream failed", status);
    std::fclose(file);
    return false;
  }

  constexpr int kChunkFrames = 256;
  std::vector<int16_t> samples(kChunkFrames * result.stream.channel_count);
  const int64_t target_frames =
      static_cast<int64_t>(result.stream.sample_rate) * duration_ms / 1000;
  long double sum_squares = 0.0;
  int peak = 0;
  while (result.stream.frames_transferred < target_frames) {
    const int frames = static_cast<int>(std::min<int64_t>(
        kChunkFrames, target_frames - result.stream.frames_transferred));
    const aaudio_result_t read = AAudioStream_read(
        guard.stream, samples.data(), frames, kIoTimeoutNanoseconds);
    if (read < 0) {
      error_ = aaudioError("read input stream failed", read);
      AAudioStream_requestStop(guard.stream);
      std::fclose(file);
      return false;
    }
    if (read == 0)
      continue;
    const size_t sample_count = read * result.stream.channel_count;
    if (std::fwrite(samples.data(), sizeof(int16_t), sample_count, file) !=
        sample_count) {
      error_ = "write WAV data failed";
      AAudioStream_requestStop(guard.stream);
      std::fclose(file);
      return false;
    }
    for (size_t index = 0; index < sample_count; ++index) {
      const int sample = samples[index];
      peak = std::max(peak, std::abs(sample));
      sum_squares += static_cast<long double>(sample) * sample;
    }
    result.stream.frames_transferred += read;
  }
  AAudioStream_requestStop(guard.stream);

  const uint64_t sample_count = static_cast<uint64_t>(
      result.stream.frames_transferred * result.stream.channel_count);
  const uint64_t byte_count = sample_count * sizeof(int16_t);
  if (byte_count > std::numeric_limits<uint32_t>::max() ||
      !writeWavHeader(file, result.stream.sample_rate,
                      result.stream.channel_count,
                      static_cast<uint32_t>(byte_count)) ||
      std::fclose(file) != 0) {
    error_ = "finalize WAV file failed";
    return false;
  }
  result.peak = static_cast<double>(peak) /
                static_cast<double>(std::numeric_limits<int16_t>::max());
  result.rms =
      sample_count == 0
          ? 0.0
          : std::sqrt(static_cast<double>(sum_squares / sample_count)) /
                std::numeric_limits<int16_t>::max();
  return true;
}

const std::string &AudioManager::lastError() const { return error_; }

} // namespace oos::hardware
