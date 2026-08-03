#pragma once

#include "oos/media/audio_decoder.h"

#include <cstdint>
#include <memory>
#include <string>

namespace oos::media {

// Sonivox-based synthesizer for SMF/SP-MIDI/XMF and Nokia-era ringtone
// formats. Its compact General MIDI wavetable is linked into OOS.
class MidiDecoder {
public:
  MidiDecoder();
  ~MidiDecoder();

  MidiDecoder(const MidiDecoder &) = delete;
  MidiDecoder &operator=(const MidiDecoder &) = delete;

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

bool isMidiOrRingtonePath(const std::string &path);

} // namespace oos::media
