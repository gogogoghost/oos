#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace oos::media {

struct DecodedAudioFormat {
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
};

// Incremental decoder for OOS's portable WAV, MP3, and FLAC baseline. Input
// storage must remain alive until close(); output is signed interleaved S16
// PCM.
class AudioDecoder {
public:
  AudioDecoder();
  ~AudioDecoder();

  AudioDecoder(const AudioDecoder &) = delete;
  AudioDecoder &operator=(const AudioDecoder &) = delete;

  bool open(const uint8_t *encoded, size_t encoded_bytes);
  bool openFile(const std::string &path);
  void close();
  bool read(int16_t *samples, uint64_t frame_capacity, uint64_t &frames_read);
  bool seek(uint64_t frame);
  uint64_t lengthFrames() const;

  const DecodedAudioFormat &format() const;
  const std::string &lastError() const;
  bool opened() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::media
