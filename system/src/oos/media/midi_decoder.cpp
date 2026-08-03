#include "oos/media/midi_decoder.h"

#include "oos/media/soundfont_midi_decoder.h"

#include <eas.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace oos::media {
namespace {

std::string easError(const char *operation, EAS_RESULT result) {
  return std::string(operation) + ": Sonivox error " + std::to_string(result);
}

std::string lowerExtension(const std::string &path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos)
    return {};
  std::string extension = path.substr(dot + 1);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  return extension;
}

bool isStandardMidiPath(const std::string &path) {
  const std::string extension = lowerExtension(path);
  return extension == "mid" || extension == "midi" || extension == "kar";
}

} // namespace

class MidiDecoder::Impl {
public:
  static int readAt(void *context, void *buffer, int offset, int size) {
    auto &decoder = *static_cast<Impl *>(context);
    if (offset < 0 || size < 0 || offset > decoder.file_size)
      return 0;
    const int available = decoder.file_size - offset;
    const int requested = std::min(size, available);
    ssize_t count;
    do {
      count = pread(decoder.fd, buffer, static_cast<size_t>(requested), offset);
    } while (count < 0 && errno == EINTR);
    return count > 0 ? static_cast<int>(count) : 0;
  }

  static int size(void *context) {
    return static_cast<Impl *>(context)->file_size;
  }

  void close() {
    if (soundfont)
      soundfont->close();
    soundfont.reset();
    if (eas && stream)
      EAS_CloseFile(eas, stream);
    stream = nullptr;
    if (eas)
      EAS_Shutdown(eas);
    eas = nullptr;
    if (fd >= 0)
      ::close(fd);
    fd = -1;
    file_size = 0;
    duration_frames = 0;
    config = nullptr;
    format = {};
  }

  int fd = -1;
  int file_size = 0;
  EAS_DATA_HANDLE eas = nullptr;
  EAS_HANDLE stream = nullptr;
  EAS_FILE locator{};
  const S_EAS_LIB_CONFIG *config = nullptr;
  DecodedAudioFormat format;
  uint64_t duration_frames = 0;
  std::string error;
  std::unique_ptr<SoundFontMidiDecoder> soundfont;
};

MidiDecoder::MidiDecoder() : impl_(std::make_unique<Impl>()) {}

MidiDecoder::~MidiDecoder() { impl_->close(); }

bool MidiDecoder::openFile(const std::string &path) {
  impl_->close();
  impl_->error.clear();
  if (isStandardMidiPath(path)) {
    impl_->soundfont = std::make_unique<SoundFontMidiDecoder>();
    if (impl_->soundfont->openFile(path))
      return true;
    impl_->error = impl_->soundfont->lastError();
    impl_->close();
    return false;
  }
  impl_->fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (impl_->fd < 0) {
    impl_->error = "open MIDI asset: " + std::string(std::strerror(errno));
    return false;
  }
  struct stat status{};
  if (fstat(impl_->fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size <= 0 || status.st_size > std::numeric_limits<int>::max()) {
    impl_->error = "MIDI asset size is invalid";
    impl_->close();
    return false;
  }
  impl_->file_size = static_cast<int>(status.st_size);
  EAS_RESULT result = EAS_Init(&impl_->eas);
  if (result != EAS_SUCCESS) {
    impl_->error = easError("initialize MIDI synthesizer", result);
    impl_->close();
    return false;
  }
  impl_->locator = {impl_.get(), Impl::readAt, Impl::size};
  result = EAS_OpenFile(impl_->eas, &impl_->locator, &impl_->stream);
  if (result == EAS_SUCCESS)
    result = EAS_Prepare(impl_->eas, impl_->stream);
  if (result != EAS_SUCCESS) {
    impl_->error = easError("prepare MIDI asset", result);
    impl_->close();
    return false;
  }
  impl_->config = EAS_Config();
  if (!impl_->config || impl_->config->sampleRate <= 0 ||
      impl_->config->numChannels <= 0 || impl_->config->numChannels > 2 ||
      impl_->config->mixBufferSize <= 0) {
    impl_->error = "MIDI synthesizer configuration is invalid";
    impl_->close();
    return false;
  }
  EAS_I32 duration_ms = 0;
  if (EAS_ParseMetaData(impl_->eas, impl_->stream, &duration_ms) ==
          EAS_SUCCESS &&
      duration_ms > 0) {
    impl_->duration_frames =
        static_cast<uint64_t>(duration_ms) * impl_->config->sampleRate / 1000;
  }
  impl_->format = {static_cast<uint32_t>(impl_->config->sampleRate),
                   static_cast<uint32_t>(impl_->config->numChannels)};
  return true;
}

void MidiDecoder::close() { impl_->close(); }

bool MidiDecoder::read(int16_t *samples, uint64_t frame_capacity,
                       uint64_t &frames_read) {
  frames_read = 0;
  if (impl_->soundfont)
    return impl_->soundfont->read(samples, frame_capacity, frames_read);
  if (!impl_->eas || !impl_->stream || !impl_->config ||
      (!samples && frame_capacity)) {
    impl_->error = "MIDI decoder is not open or output is invalid";
    return false;
  }
  while (frames_read < frame_capacity) {
    EAS_STATE state = EAS_STATE_ERROR;
    EAS_RESULT result = EAS_State(impl_->eas, impl_->stream, &state);
    if (result != EAS_SUCCESS || state == EAS_STATE_ERROR) {
      impl_->error = easError("query MIDI state", result);
      return false;
    }
    if (state == EAS_STATE_STOPPED)
      break;
    const uint64_t remaining = frame_capacity - frames_read;
    const EAS_I32 requested = static_cast<EAS_I32>(std::min<uint64_t>(
        remaining, static_cast<uint64_t>(impl_->config->mixBufferSize)));
    EAS_I32 generated = 0;
    result =
        EAS_Render(impl_->eas, samples + frames_read * impl_->format.channels,
                   requested, &generated);
    if (result != EAS_SUCCESS || generated < 0 || generated > requested) {
      impl_->error = easError("render MIDI", result);
      return false;
    }
    frames_read += static_cast<uint64_t>(generated);
    if (generated == 0)
      break;
  }
  return true;
}

bool MidiDecoder::seek(uint64_t frame) {
  if (impl_->soundfont)
    return impl_->soundfont->seek(frame);
  if (!impl_->eas || !impl_->stream || !impl_->config) {
    impl_->error = "MIDI decoder is not open";
    return false;
  }
  const uint64_t milliseconds = frame * 1000 / impl_->config->sampleRate;
  if (milliseconds >
      static_cast<uint64_t>(std::numeric_limits<EAS_I32>::max())) {
    impl_->error = "MIDI seek position is too large";
    return false;
  }
  const EAS_RESULT result = EAS_Locate(
      impl_->eas, impl_->stream, static_cast<EAS_I32>(milliseconds), EAS_FALSE);
  if (result == EAS_SUCCESS)
    return true;
  impl_->error = easError("seek MIDI", result);
  return false;
}

uint64_t MidiDecoder::lengthFrames() const {
  return impl_->soundfont ? impl_->soundfont->lengthFrames()
                          : impl_->duration_frames;
}

const DecodedAudioFormat &MidiDecoder::format() const {
  return impl_->soundfont ? impl_->soundfont->format() : impl_->format;
}

const std::string &MidiDecoder::lastError() const {
  return impl_->soundfont ? impl_->soundfont->lastError() : impl_->error;
}

bool MidiDecoder::opened() const {
  return impl_->soundfont ? impl_->soundfont->opened()
                          : impl_->eas && impl_->stream;
}

bool isMidiOrRingtonePath(const std::string &path) {
  const std::string extension = lowerExtension(path);
  return extension == "mid" || extension == "midi" || extension == "kar" ||
         extension == "xmf" || extension == "mxmf" || extension == "imy" ||
         extension == "rtttl" || extension == "ota";
}

} // namespace oos::media
