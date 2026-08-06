#include "oos/media/soundfont_midi_decoder.h"

#include "oos/media/embedded_soundfont.h"

#include <fluidlite.h>

#define TML_IMPLEMENTATION
#include <tml.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <vector>

namespace oos::media {
namespace {

constexpr uint32_t kSampleRate = 44100;
constexpr uint32_t kChannels = 2;
constexpr uint32_t kMaximumVoices = 64;
constexpr uint32_t kReleaseTailMilliseconds = 3000;
constexpr char kEmbeddedSoundFontName[] = "gm.sf2";
constexpr int kFluidOk = 0;
constexpr int kFluidFailed = -1;

struct MemoryCursor {
  size_t offset = 0;
};

void *memoryOpen(fluid_fileapi_t *, const char *filename) {
  if (!filename || std::strcmp(filename, kEmbeddedSoundFontName) != 0)
    return nullptr;
  return new (std::nothrow) MemoryCursor();
}

int memoryRead(void *output, int count, void *handle) {
  auto *cursor = static_cast<MemoryCursor *>(handle);
  const size_t size = embeddedGmSoundFontSize();
  if (!cursor || !output || count < 0 || cursor->offset > size ||
      static_cast<size_t>(count) > size - cursor->offset)
    return kFluidFailed;
  std::memcpy(output, embeddedGmSoundFontData() + cursor->offset,
              static_cast<size_t>(count));
  cursor->offset += static_cast<size_t>(count);
  return kFluidOk;
}

int memorySeek(void *handle, long offset, int origin) {
  auto *cursor = static_cast<MemoryCursor *>(handle);
  if (!cursor)
    return kFluidFailed;
  const int64_t size = static_cast<int64_t>(embeddedGmSoundFontSize());
  int64_t base = 0;
  if (origin == SEEK_CUR)
    base = static_cast<int64_t>(cursor->offset);
  else if (origin == SEEK_END)
    base = size;
  else if (origin != SEEK_SET)
    return kFluidFailed;
  const int64_t position = base + static_cast<int64_t>(offset);
  if (position < 0 || position > size)
    return kFluidFailed;
  cursor->offset = static_cast<size_t>(position);
  return kFluidOk;
}

int memoryClose(void *handle) {
  delete static_cast<MemoryCursor *>(handle);
  return kFluidOk;
}

long memoryTell(void *handle) {
  auto *cursor = static_cast<MemoryCursor *>(handle);
  if (!cursor || cursor->offset > static_cast<size_t>(LONG_MAX))
    return kFluidFailed;
  return static_cast<long>(cursor->offset);
}

const void *memoryMap(void *handle, long offset, int count) {
  auto *cursor = static_cast<MemoryCursor *>(handle);
  const size_t size = embeddedGmSoundFontSize();
  if (!cursor || offset < 0 || count < 0 ||
      static_cast<size_t>(offset) > size ||
      static_cast<size_t>(count) > size - static_cast<size_t>(offset))
    return nullptr;
  return embeddedGmSoundFontData() + static_cast<size_t>(offset);
}

class SharedSoundFont {
public:
  fluid_sfont_t *acquire(std::string &error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!soundfont_ && !load(error))
      return nullptr;
    ++references_;
    return soundfont_;
  }

  void release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!soundfont_ || references_ == 0)
      return;
    if (--references_ == 0) {
      soundfont_->free(soundfont_);
      soundfont_ = nullptr;
    }
  }

private:
  bool load(std::string &error) {
    fluid_sfloader_t *loader = new_fluid_defsfloader();
    if (!loader) {
      error = "create embedded SoundFont loader failed";
      return false;
    }
    fluid_fileapi_t api = {nullptr,    nullptr,     memoryOpen, memoryRead,
                           memorySeek, memoryClose, memoryTell, memoryMap};
    loader->fileapi = &api;
    soundfont_ = loader->load(loader, kEmbeddedSoundFontName);
    loader->fileapi = nullptr;
    delete_fluid_defsfloader(loader);
    if (soundfont_)
      return true;
    error = "load embedded gm.sf2 failed";
    return false;
  }

  std::mutex mutex_;
  fluid_sfont_t *soundfont_ = nullptr;
  size_t references_ = 0;
};

SharedSoundFont &sharedSoundFont() {
  static SharedSoundFont soundfont;
  return soundfont;
}

uint64_t frameForMilliseconds(uint32_t milliseconds) {
  return static_cast<uint64_t>(milliseconds) * kSampleRate / 1000;
}

} // namespace

class SoundFontMidiDecoder::Impl {
public:
  void close() {
    if (synth && soundfont_attached)
      fluid_synth_remove_sfont(synth, soundfont);
    soundfont_attached = false;
    if (synth)
      delete_fluid_synth(synth);
    synth = nullptr;
    if (soundfont)
      sharedSoundFont().release();
    soundfont = nullptr;
    if (settings)
      delete_fluid_settings(settings);
    settings = nullptr;
    if (messages)
      tml_free(messages);
    messages = nullptr;
    next_message = nullptr;
    current_frame = 0;
    duration_frames = 0;
    ended = false;
    format = {};
  }

  bool reset() {
    if (!synth || fluid_synth_system_reset(synth) != kFluidOk)
      return false;
    next_message = messages;
    current_frame = 0;
    ended = false;
    return true;
  }

  bool configure(unsigned int duration_ms) {
    settings = new_fluid_settings();
    if (!settings ||
        fluid_settings_setnum(settings, "synth.sample-rate", kSampleRate) ==
            0 ||
        fluid_settings_setint(settings, "synth.polyphony", kMaximumVoices) ==
            0 ||
        fluid_settings_setstr(settings, "synth.chorus.active", "no") != 0) {
      error = "configure FluidLite MIDI synthesizer failed";
      return false;
    }
    synth = new_fluid_synth(settings);
    if (!synth) {
      error = "create FluidLite MIDI synthesizer failed";
      return false;
    }
    soundfont = sharedSoundFont().acquire(error);
    if (!soundfont)
      return false;
    if (fluid_synth_add_sfont(synth, soundfont) < 0) {
      error = "attach embedded gm.sf2 failed";
      return false;
    }
    soundfont_attached = true;
    if (fluid_synth_set_interp_method(synth, -1, FLUID_INTERP_LINEAR) !=
        kFluidOk) {
      error = "configure SoundFont interpolation failed";
      return false;
    }
    next_message = messages;
    duration_frames =
        (static_cast<uint64_t>(duration_ms) + kReleaseTailMilliseconds) *
        kSampleRate / 1000;
    format = {kSampleRate, kChannels};
    return true;
  }

  void dispatch(const tml_message &message) {
    const int channel = message.channel;
    const int first = static_cast<unsigned char>(message.key);
    const int second = static_cast<unsigned char>(message.velocity);
    switch (message.type) {
    case TML_NOTE_ON:
      if (second)
        fluid_synth_noteon(synth, channel, first, second);
      else
        fluid_synth_noteoff(synth, channel, first);
      break;
    case TML_NOTE_OFF:
      fluid_synth_noteoff(synth, channel, first);
      break;
    case TML_KEY_PRESSURE:
      fluid_synth_key_pressure(synth, channel, first, second);
      break;
    case TML_CONTROL_CHANGE:
      fluid_synth_cc(synth, channel, first, second);
      break;
    case TML_PROGRAM_CHANGE:
      fluid_synth_program_change(synth, channel, first);
      break;
    case TML_CHANNEL_PRESSURE:
      fluid_synth_channel_pressure(synth, channel, first);
      break;
    case TML_PITCH_BEND:
      fluid_synth_pitch_bend(synth, channel, message.pitch_bend);
      break;
    default:
      break;
    }
  }

  bool render(int16_t *samples, uint64_t capacity, uint64_t &frames) {
    frames = 0;
    while (frames < capacity && !ended) {
      while (next_message &&
             frameForMilliseconds(next_message->time) <= current_frame) {
        dispatch(*next_message);
        next_message = next_message->next;
      }
      if (!next_message && fluid_synth_get_active_voice_count(synth) == 0) {
        ended = true;
        break;
      }
      const uint64_t event_frame =
          next_message ? frameForMilliseconds(next_message->time)
                       : duration_frames;
      const uint64_t available = capacity - frames;
      const uint64_t until_event =
          event_frame > current_frame ? event_frame - current_frame : 1;
      const uint64_t count = std::min(available, until_event);
      if (current_frame >= duration_frames || count == 0) {
        ended = true;
        break;
      }
      const int render_count = static_cast<int>(std::min<uint64_t>(
          count, static_cast<uint64_t>(std::numeric_limits<int>::max())));
      int16_t *output = samples + frames * kChannels;
      if (fluid_synth_write_s16(synth, render_count, output, 0, 2, output, 1,
                                2) != kFluidOk) {
        error = "FluidLite failed to render MIDI audio";
        return false;
      }
      frames += static_cast<uint64_t>(render_count);
      current_frame += static_cast<uint64_t>(render_count);
    }
    return true;
  }

  fluid_settings_t *settings = nullptr;
  fluid_synth_t *synth = nullptr;
  fluid_sfont_t *soundfont = nullptr;
  bool soundfont_attached = false;
  tml_message *messages = nullptr;
  tml_message *next_message = nullptr;
  uint64_t current_frame = 0;
  uint64_t duration_frames = 0;
  bool ended = false;
  DecodedAudioFormat format;
  std::string error;
};

SoundFontMidiDecoder::SoundFontMidiDecoder()
    : impl_(std::make_unique<Impl>()) {}

SoundFontMidiDecoder::~SoundFontMidiDecoder() { impl_->close(); }

bool SoundFontMidiDecoder::openFile(const std::string &path) {
  impl_->close();
  impl_->error.clear();
  impl_->messages = tml_load_filename(path.c_str());
  if (!impl_->messages) {
    impl_->error = "parse standard MIDI asset failed";
    return false;
  }
  unsigned int duration_ms = 0;
  tml_get_info(impl_->messages, nullptr, nullptr, nullptr, nullptr,
               &duration_ms);
  if (impl_->configure(duration_ms))
    return true;
  impl_->close();
  return false;
}

bool SoundFontMidiDecoder::open(const uint8_t *encoded, size_t encoded_bytes) {
  impl_->close();
  impl_->error.clear();
  if (!encoded || encoded_bytes == 0 || encoded_bytes > UINT_MAX) {
    impl_->error = "standard MIDI byte source is invalid";
    return false;
  }
  impl_->messages = tml_load_memory(const_cast<uint8_t *>(encoded),
                                    static_cast<int>(encoded_bytes));
  if (!impl_->messages) {
    impl_->error = "parse standard MIDI byte source failed";
    return false;
  }
  unsigned int duration_ms = 0;
  tml_get_info(impl_->messages, nullptr, nullptr, nullptr, nullptr,
               &duration_ms);
  if (impl_->configure(duration_ms))
    return true;
  impl_->close();
  return false;
}

void SoundFontMidiDecoder::close() { impl_->close(); }

bool SoundFontMidiDecoder::read(int16_t *samples, uint64_t frame_capacity,
                                uint64_t &frames_read) {
  frames_read = 0;
  if (!impl_->synth || (!samples && frame_capacity)) {
    impl_->error = "SoundFont MIDI decoder is not open or output is invalid";
    return false;
  }
  return impl_->render(samples, frame_capacity, frames_read);
}

bool SoundFontMidiDecoder::seek(uint64_t frame) {
  if (!impl_->synth || frame > impl_->duration_frames || !impl_->reset()) {
    impl_->error = "SoundFont MIDI seek position is invalid";
    return false;
  }
  std::vector<int16_t> scratch(512 * kChannels);
  while (impl_->current_frame < frame) {
    uint64_t generated = 0;
    const uint64_t requested =
        std::min<uint64_t>(512, frame - impl_->current_frame);
    if (!impl_->render(scratch.data(), requested, generated) ||
        generated != requested) {
      impl_->error = "SoundFont MIDI seek failed";
      return false;
    }
  }
  return true;
}

uint64_t SoundFontMidiDecoder::lengthFrames() const {
  return impl_->duration_frames;
}

const DecodedAudioFormat &SoundFontMidiDecoder::format() const {
  return impl_->format;
}

const std::string &SoundFontMidiDecoder::lastError() const {
  return impl_->error;
}

bool SoundFontMidiDecoder::opened() const { return impl_->synth != nullptr; }

} // namespace oos::media
