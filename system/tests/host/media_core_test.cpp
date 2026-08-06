#include "oos/media/audio_decoder.h"
#include "oos/media/audio_format.h"
#include "oos/media/ffmpeg_decoder.h"
#include "oos/media/media_source.h"
#include "oos/media/midi_decoder.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unistd.h>
#include <vector>

namespace {

void little16(std::vector<uint8_t> &bytes, uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void little32(std::vector<uint8_t> &bytes, uint32_t value) {
  little16(bytes, static_cast<uint16_t>(value));
  little16(bytes, static_cast<uint16_t>(value >> 16));
}

std::vector<uint8_t> makeWav() {
  constexpr std::array<int16_t, 4> samples = {-32768, -1, 0, 32767};
  std::vector<uint8_t> bytes;
  const auto append = [&bytes](const char *value) {
    bytes.insert(bytes.end(), value, value + 4);
  };
  append("RIFF");
  little32(bytes, 36 + samples.size() * sizeof(int16_t));
  append("WAVE");
  append("fmt ");
  little32(bytes, 16);
  little16(bytes, 1);
  little16(bytes, 1);
  little32(bytes, 8000);
  little32(bytes, 16000);
  little16(bytes, 2);
  little16(bytes, 16);
  append("data");
  little32(bytes, samples.size() * sizeof(int16_t));
  const auto *sample_bytes = reinterpret_cast<const uint8_t *>(samples.data());
  bytes.insert(bytes.end(), sample_bytes,
               sample_bytes + samples.size() * sizeof(int16_t));
  return bytes;
}

} // namespace

int main(int argc, char **argv) {
  const auto &formats = oos::media::supportedAudioFormats();
  assert(formats.size() >= 9);
  assert(oos::media::findAudioFormat("audio/wav"));
  assert(oos::media::findAudioFormat("audio/x-wav"));
  assert(oos::media::findAudioFormat("audio/midi"));

  std::vector<uint8_t> wav = makeWav();
  oos::media::AudioDecoder decoder;
  assert(decoder.open(wav.data(), wav.size()));
  assert(decoder.format().sample_rate == 8000);
  assert(decoder.format().channels == 1);
  std::array<int16_t, 8> output{};
  uint64_t frames = 0;
  assert(decoder.read(output.data(), output.size(), frames));
  assert(frames == 4);
  assert(output[0] == -32768 && output[3] == 32767);
  assert(decoder.seek(2));
  assert(decoder.read(output.data(), 1, frames));
  assert(frames == 1 && output[0] == 0);
  decoder.close();

  const std::array<uint8_t, 37> midi = {
      'M',  'T', 'h', 'd', 0,    0,  0,  6, 0,    0,    0,    1, 0,
      96,   'M', 'T', 'r', 'k',  0,  0,  0, 15,   0,    0xc0, 0, 0,
      0x90, 60,  100, 96,  0x80, 60, 64, 0, 0xff, 0x2f, 0};
  assert(oos::media::identifyMedia(midi.data(), midi.size(),
                                   "application/octet-stream", "bad.bin") ==
         oos::media::MediaDecoderKind::StandardMidi);
  char midi_path[] = "/tmp/oos-midi-test.XXXXXX.mid";
  const int midi_fd = mkstemps(midi_path, 4);
  assert(midi_fd >= 0);
  assert(write(midi_fd, midi.data(), midi.size()) ==
         static_cast<ssize_t>(midi.size()));
  close(midi_fd);
  oos::media::MidiDecoder midi_decoder;
  assert(midi_decoder.open(midi.data(), midi.size(), true));
  oos::media::MidiDecoder second_midi_decoder;
  assert(second_midi_decoder.openFile(midi_path));
  assert(midi_decoder.format().sample_rate == 44100);
  assert(midi_decoder.format().channels == 2);
  std::vector<int16_t> midi_pcm(1024 * 2);
  assert(midi_decoder.read(midi_pcm.data(), 1024, frames));
  assert(frames > 0);
  assert(std::any_of(midi_pcm.begin(), midi_pcm.end(),
                     [](int16_t sample) { return sample != 0; }));
  assert(second_midi_decoder.read(midi_pcm.data(), 1024, frames));
  assert(frames > 0);
  second_midi_decoder.close();
  assert(midi_decoder.seek(0));
  midi_decoder.close();
  unlink(midi_path);

  constexpr std::array<uint8_t, 4> invalid = {0, 1, 2, 3};
  assert(oos::media::identifyMedia(invalid.data(), invalid.size(), "", "") ==
         oos::media::MediaDecoderKind::Unknown);
  assert(!decoder.open(invalid.data(), invalid.size()));
  char invalid_path[] = "/tmp/oos-invalid-audio.XXXXXX.aac";
  const int invalid_fd = mkstemps(invalid_path, 4);
  assert(invalid_fd >= 0);
  assert(write(invalid_fd, invalid.data(), invalid.size()) ==
         static_cast<ssize_t>(invalid.size()));
  close(invalid_fd);
  oos::media::FfmpegDecoder invalid_decoder;
  assert(!invalid_decoder.open(invalid.data(), invalid.size()));
  assert(!invalid_decoder.openFile(invalid_path));
  unlink(invalid_path);

  for (int argument = 1; argument < argc; ++argument) {
    if (oos::media::isMidiOrRingtonePath(argv[argument])) {
      oos::media::MidiDecoder legacy_decoder;
      assert(legacy_decoder.openFile(argv[argument]));
      std::vector<int16_t> decoded(4096 * legacy_decoder.format().channels);
      assert(legacy_decoder.read(decoded.data(), 4096, frames));
      assert(frames > 0);
      legacy_decoder.close();
      continue;
    }
    if (oos::media::isFfmpegAudioPath(argv[argument])) {
      oos::media::FfmpegDecoder ffmpeg_decoder;
      assert(ffmpeg_decoder.openFile(argv[argument]));
      assert(ffmpeg_decoder.format().sample_rate >= 8000);
      assert(ffmpeg_decoder.format().channels >= 1);
      std::vector<int16_t> decoded(4096 * ffmpeg_decoder.format().channels);
      assert(ffmpeg_decoder.read(decoded.data(), 4096, frames));
      assert(frames > 0);
      assert(ffmpeg_decoder.seek(0));
      ffmpeg_decoder.close();
      continue;
    }
    std::ifstream input(argv[argument], std::ios::binary);
    assert(input);
    std::vector<uint8_t> encoded{std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>()};
    assert(decoder.open(encoded.data(), encoded.size()));
    std::vector<int16_t> decoded(4096 * decoder.format().channels);
    assert(decoder.read(decoded.data(), 4096, frames));
    assert(frames > 0);
    decoder.close();
  }
  return 0;
}
