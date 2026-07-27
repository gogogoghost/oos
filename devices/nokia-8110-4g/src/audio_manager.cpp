#include "oos/hardware/audio_manager.h"
#include "oos/device/nokia8110/permission_controller.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <mutex>
#include <vector>

namespace oos::hardware {
namespace {

constexpr int kSampleRate = 48000;
constexpr int kRecordSampleRate = 16000;
constexpr int kChannels = 1;
constexpr double kPi = 3.14159265358979323846;

class OpenSlObject {
public:
  ~OpenSlObject() {
    if (object_)
      (*object_)->Destroy(object_);
  }

  SLObjectItf *out() { return &object_; }
  SLObjectItf get() const { return object_; }

private:
  SLObjectItf object_ = nullptr;
};

class Completion {
public:
  static void callback(SLAndroidSimpleBufferQueueItf, void *context) {
    auto *completion = static_cast<Completion *>(context);
    {
      std::lock_guard<std::mutex> lock(completion->mutex_);
      completion->done_ = true;
    }
    completion->condition_.notify_one();
  }

  bool wait(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [this] { return done_; });
  }

  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = false;
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool done_ = false;
};

std::string slError(const char *operation, SLresult result) {
  return std::string(operation) + ": OpenSL ES result " +
         std::to_string(result);
}

SLint32 streamType(AudioUsage usage) {
  switch (usage) {
  case AudioUsage::Voice:
    return SL_ANDROID_STREAM_VOICE;
  case AudioUsage::Ringtone:
    return SL_ANDROID_STREAM_RING;
  case AudioUsage::Alarm:
    return SL_ANDROID_STREAM_ALARM;
  case AudioUsage::Notification:
    return SL_ANDROID_STREAM_NOTIFICATION;
  case AudioUsage::Media:
    return SL_ANDROID_STREAM_MEDIA;
  }
  return SL_ANDROID_STREAM_MEDIA;
}

bool createEngine(OpenSlObject &engine, SLEngineItf &interface,
                  std::string &error) {
  SLresult result =
      slCreateEngine(engine.out(), 0, nullptr, 0, nullptr, nullptr);
  if (result != SL_RESULT_SUCCESS) {
    error = slError("create engine failed", result);
    return false;
  }
  result = (*engine.get())->Realize(engine.get(), SL_BOOLEAN_FALSE);
  if (result != SL_RESULT_SUCCESS) {
    error = slError("realize engine failed", result);
    return false;
  }
  result =
      (*engine.get())->GetInterface(engine.get(), SL_IID_ENGINE, &interface);
  if (result != SL_RESULT_SUCCESS) {
    error = slError("get engine interface failed", result);
    return false;
  }
  return true;
}

SLDataFormat_PCM pcmFormat(int sample_rate) {
  return {SL_DATAFORMAT_PCM,
          kChannels,
          static_cast<SLuint32>(sample_rate * 1000),
          SL_PCMSAMPLEFORMAT_FIXED_16,
          SL_PCMSAMPLEFORMAT_FIXED_16,
          SL_SPEAKER_FRONT_CENTER,
          SL_BYTEORDER_LITTLEENDIAN};
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

} // namespace

bool AudioManager::playTone(double frequency_hz, int duration_ms, float volume,
                            AudioUsage usage, AudioStreamInfo &info) {
  info = {};
  error_.clear();
  if (frequency_hz <= 0.0 || duration_ms <= 0 || volume < 0.0F ||
      volume > 1.0F) {
    error_ = "invalid tone parameters";
    return false;
  }

  OpenSlObject engine;
  SLEngineItf engine_interface = nullptr;
  if (!createEngine(engine, engine_interface, error_))
    return false;

  OpenSlObject output_mix;
  SLresult result = (*engine_interface)
                        ->CreateOutputMix(engine_interface, output_mix.out(), 0,
                                          nullptr, nullptr);
  if (result != SL_RESULT_SUCCESS) {
    error_ = slError("create output mix failed", result);
    return false;
  }
  result = (*output_mix.get())->Realize(output_mix.get(), SL_BOOLEAN_FALSE);
  if (result != SL_RESULT_SUCCESS) {
    error_ = slError("realize output mix failed", result);
    return false;
  }

  SLDataLocator_AndroidSimpleBufferQueue queue_locator = {
      SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1};
  SLDataFormat_PCM format = pcmFormat(kSampleRate);
  SLDataSource source = {&queue_locator, &format};
  SLDataLocator_OutputMix output_locator = {SL_DATALOCATOR_OUTPUTMIX,
                                            output_mix.get()};
  SLDataSink sink = {&output_locator, nullptr};
  const SLInterfaceID interfaces[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                      SL_IID_ANDROIDCONFIGURATION};
  const SLboolean required[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
  OpenSlObject player;
  result = (*engine_interface)
               ->CreateAudioPlayer(engine_interface, player.out(), &source,
                                   &sink, 2, interfaces, required);
  if (result != SL_RESULT_SUCCESS) {
    error_ = slError("create audio player failed", result);
    return false;
  }

  SLAndroidConfigurationItf configuration = nullptr;
  result = (*player.get())
               ->GetInterface(player.get(), SL_IID_ANDROIDCONFIGURATION,
                              &configuration);
  if (result != SL_RESULT_SUCCESS) {
    error_ = slError("get player configuration failed", result);
    return false;
  }
  const SLint32 android_stream = streamType(usage);
  result = (*configuration)
               ->SetConfiguration(configuration, SL_ANDROID_KEY_STREAM_TYPE,
                                  &android_stream, sizeof(android_stream));
  if (result != SL_RESULT_SUCCESS) {
    error_ = slError("set playback stream failed", result);
    return false;
  }
  result = (*player.get())->Realize(player.get(), SL_BOOLEAN_FALSE);
  if (result != SL_RESULT_SUCCESS) {
    error_ = slError("realize audio player failed", result);
    return false;
  }

  SLPlayItf play = nullptr;
  SLAndroidSimpleBufferQueueItf queue = nullptr;
  if ((result =
           (*player.get())->GetInterface(player.get(), SL_IID_PLAY, &play)) !=
          SL_RESULT_SUCCESS ||
      (result = (*player.get())
                    ->GetInterface(player.get(),
                                   SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &queue)) !=
          SL_RESULT_SUCCESS) {
    error_ = slError("get player interface failed", result);
    return false;
  }

  const int64_t target_frames =
      static_cast<int64_t>(kSampleRate) * duration_ms / 1000;
  std::vector<int16_t> samples(static_cast<size_t>(target_frames));
  const double phase_step = 2.0 * kPi * frequency_hz / kSampleRate;
  const double amplitude = std::numeric_limits<int16_t>::max() * volume;
  for (int64_t frame = 0; frame < target_frames; ++frame)
    samples[frame] =
        static_cast<int16_t>(std::sin(frame * phase_step) * amplitude);

  Completion completion;
  result = (*queue)->RegisterCallback(queue, Completion::callback, &completion);
  if (result == SL_RESULT_SUCCESS)
    result = (*queue)->Enqueue(queue, samples.data(),
                               samples.size() * sizeof(int16_t));
  if (result == SL_RESULT_SUCCESS)
    result = (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING);
  if (result != SL_RESULT_SUCCESS) {
    error_ = slError("start playback failed", result);
    return false;
  }
  if (!completion.wait(duration_ms + 3000)) {
    (*play)->SetPlayState(play, SL_PLAYSTATE_STOPPED);
    error_ = "playback timed out";
    return false;
  }
  (*play)->SetPlayState(play, SL_PLAYSTATE_STOPPED);
  info.sample_rate = kSampleRate;
  info.channel_count = kChannels;
  info.frames_transferred = target_frames;
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
  if (!oos::device::nokia8110::ensurePermissionController(error_))
    return false;

  OpenSlObject engine;
  SLEngineItf engine_interface = nullptr;
  if (!createEngine(engine, engine_interface, error_))
    return false;

  SLDataLocator_IODevice input_locator = {
      SL_DATALOCATOR_IODEVICE, SL_IODEVICE_AUDIOINPUT,
      SL_DEFAULTDEVICEID_AUDIOINPUT, nullptr};
  SLDataSource source = {&input_locator, nullptr};
  SLDataLocator_AndroidSimpleBufferQueue queue_locator = {
      SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1};
  SLDataFormat_PCM format = pcmFormat(kRecordSampleRate);
  SLDataSink sink = {&queue_locator, &format};
  const SLInterfaceID interfaces[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                      SL_IID_ANDROIDCONFIGURATION};
  const SLboolean required[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
  OpenSlObject recorder;
  SLresult sl_result =
      (*engine_interface)
          ->CreateAudioRecorder(engine_interface, recorder.out(), &source,
                                &sink, 2, interfaces, required);
  if (sl_result != SL_RESULT_SUCCESS) {
    error_ = slError("create audio recorder failed", sl_result);
    return false;
  }
  SLAndroidConfigurationItf configuration = nullptr;
  sl_result = (*recorder.get())
                  ->GetInterface(recorder.get(), SL_IID_ANDROIDCONFIGURATION,
                                 &configuration);
  if (sl_result != SL_RESULT_SUCCESS) {
    error_ = slError("get recorder configuration failed", sl_result);
    return false;
  }
  const SLuint32 preset = SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION;
  sl_result =
      (*configuration)
          ->SetConfiguration(configuration, SL_ANDROID_KEY_RECORDING_PRESET,
                             &preset, sizeof(preset));
  if (sl_result != SL_RESULT_SUCCESS) {
    error_ = slError("set recording preset failed", sl_result);
    return false;
  }
  sl_result = (*recorder.get())->Realize(recorder.get(), SL_BOOLEAN_FALSE);
  if (sl_result != SL_RESULT_SUCCESS) {
    error_ = slError("realize audio recorder failed", sl_result);
    return false;
  }

  SLRecordItf record = nullptr;
  SLAndroidSimpleBufferQueueItf queue = nullptr;
  if ((sl_result = (*recorder.get())
                       ->GetInterface(recorder.get(), SL_IID_RECORD,
                                      &record)) != SL_RESULT_SUCCESS ||
      (sl_result =
           (*recorder.get())
               ->GetInterface(recorder.get(), SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                              &queue)) != SL_RESULT_SUCCESS) {
    error_ = slError("get recorder interface failed", sl_result);
    return false;
  }

  const int64_t target_frames =
      static_cast<int64_t>(kRecordSampleRate) * duration_ms / 1000;
  std::vector<int16_t> samples;
  samples.reserve(static_cast<size_t>(target_frames));
  constexpr size_t kCaptureChunkFrames = 1024;
  std::array<int16_t, kCaptureChunkFrames> capture_chunk{};
  Completion completion;
  sl_result =
      (*queue)->RegisterCallback(queue, Completion::callback, &completion);
  if (sl_result != SL_RESULT_SUCCESS) {
    error_ = slError("register recording callback failed", sl_result);
    return false;
  }

  bool started = false;
  while (samples.size() < static_cast<size_t>(target_frames)) {
    const size_t frames =
        std::min(kCaptureChunkFrames,
                 static_cast<size_t>(target_frames) - samples.size());
    completion.reset();
    sl_result = (*queue)->Enqueue(queue, capture_chunk.data(),
                                  frames * sizeof(int16_t));
    if (sl_result == SL_RESULT_SUCCESS && !started) {
      sl_result = (*record)->SetRecordState(record, SL_RECORDSTATE_RECORDING);
      started = sl_result == SL_RESULT_SUCCESS;
    }
    if (sl_result != SL_RESULT_SUCCESS) {
      if (started)
        (*record)->SetRecordState(record, SL_RECORDSTATE_STOPPED);
      error_ = slError("capture audio buffer failed", sl_result);
      return false;
    }
    if (!completion.wait(1000)) {
      (*record)->SetRecordState(record, SL_RECORDSTATE_STOPPED);
      error_ = "recording buffer timed out";
      return false;
    }
    samples.insert(samples.end(), capture_chunk.begin(),
                   capture_chunk.begin() + frames);
  }
  (*record)->SetRecordState(record, SL_RECORDSTATE_STOPPED);

  std::FILE *file = std::fopen(path.c_str(), "wb");
  if (!file) {
    error_ = "open WAV output failed";
    return false;
  }
  const uint64_t byte_count = samples.size() * sizeof(int16_t);
  const bool wrote = byte_count <= std::numeric_limits<uint32_t>::max() &&
                     writeWavHeader(file, kRecordSampleRate, kChannels,
                                    static_cast<uint32_t>(byte_count)) &&
                     std::fseek(file, 0, SEEK_END) == 0 &&
                     std::fwrite(samples.data(), sizeof(int16_t),
                                 samples.size(), file) == samples.size();
  if (!wrote || std::fclose(file) != 0) {
    error_ = "write WAV output failed";
    return false;
  }

  long double sum_squares = 0.0;
  int peak = 0;
  for (int16_t sample : samples) {
    peak = std::max(peak, std::abs(static_cast<int>(sample)));
    sum_squares += static_cast<long double>(sample) * sample;
  }
  result.path = path;
  result.stream.sample_rate = kRecordSampleRate;
  result.stream.channel_count = kChannels;
  result.stream.frames_transferred = target_frames;
  result.peak = static_cast<double>(peak) /
                static_cast<double>(std::numeric_limits<int16_t>::max());
  result.rms =
      samples.empty()
          ? 0.0
          : std::sqrt(static_cast<double>(sum_squares / samples.size())) /
                std::numeric_limits<int16_t>::max();
  return true;
}

const std::string &AudioManager::lastError() const { return error_; }

} // namespace oos::hardware
