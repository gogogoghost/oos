#pragma once

#include "oos/media/audio_decoder.h"

#include <cstdint>
#include <memory>
#include <string>

namespace oos::media {

class SoundFontMidiDecoder {
public:
  SoundFontMidiDecoder();
  ~SoundFontMidiDecoder();

  SoundFontMidiDecoder(const SoundFontMidiDecoder &) = delete;
  SoundFontMidiDecoder &operator=(const SoundFontMidiDecoder &) = delete;

  bool openFile(const std::string &path);
  bool open(const uint8_t *encoded, size_t encoded_bytes);
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
