#include "oos/media/media_source.h"

#include "oos/media/ffmpeg_decoder.h"
#include "oos/media/midi_decoder.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace oos::media {
namespace {

bool startsWith(const uint8_t *bytes, size_t size, const char *magic,
                size_t magic_size) {
  return size >= magic_size && std::memcmp(bytes, magic, magic_size) == 0;
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

bool endsWith(const std::string &value, const char *suffix) {
  const size_t length = std::strlen(suffix);
  return value.size() >= length &&
         value.compare(value.size() - length, length, suffix) == 0;
}

MediaDecoderKind fromMime(const std::string &mime_type) {
  const std::string mime = lowercase(mime_type);
  if (mime == "audio/midi" || mime == "audio/sp-midi" || mime == "audio/x-midi")
    return MediaDecoderKind::StandardMidi;
  if (mime == "audio/mobile-xmf" || mime == "audio/imelody" ||
      mime == "audio/rtttl" || mime == "audio/vnd.nokia.ringing-tone")
    return MediaDecoderKind::LegacyMidi;
  if (mime == "audio/wav" || mime == "audio/x-wav" || mime == "audio/mpeg" ||
      mime == "audio/mp3" || mime == "audio/flac" || mime == "audio/x-flac")
    return MediaDecoderKind::Portable;
  if (mime == "audio/aac" || mime == "audio/mp4" || mime == "audio/3gpp" ||
      mime == "audio/amr" || mime == "audio/amr-wb" || mime == "audio/ogg" ||
      mime == "audio/opus" || mime.rfind("audio/ogg;", 0) == 0)
    return MediaDecoderKind::Ffmpeg;
  return MediaDecoderKind::Unknown;
}

MediaDecoderKind fromHint(const std::string &hint) {
  if (isMidiOrRingtonePath(hint)) {
    const std::string lower = lowercase(hint);
    return (endsWith(lower, ".mid") || endsWith(lower, ".midi") ||
            endsWith(lower, ".kar"))
               ? MediaDecoderKind::StandardMidi
               : MediaDecoderKind::LegacyMidi;
  }
  if (isFfmpegAudioPath(hint))
    return MediaDecoderKind::Ffmpeg;
  const std::string lower = lowercase(hint);
  if (endsWith(lower, ".wav") || endsWith(lower, ".mp3") ||
      endsWith(lower, ".flac"))
    return MediaDecoderKind::Portable;
  return MediaDecoderKind::Unknown;
}

} // namespace

MediaDecoderKind identifyMedia(const uint8_t *bytes, size_t size,
                               const std::string &mime_type,
                               const std::string &locator_hint) {
  if (!bytes || size == 0)
    return MediaDecoderKind::Unknown;
  if (startsWith(bytes, size, "MThd", 4))
    return MediaDecoderKind::StandardMidi;
  if (startsWith(bytes, size, "XMF_", 4) ||
      startsWith(bytes, size, "BEGIN:IMELODY", 13))
    return MediaDecoderKind::LegacyMidi;
  if ((startsWith(bytes, size, "RIFF", 4) && size >= 12 &&
       std::memcmp(bytes + 8, "WAVE", 4) == 0) ||
      startsWith(bytes, size, "fLaC", 4) || startsWith(bytes, size, "ID3", 3) ||
      (size >= 2 && bytes[0] == 0xff && (bytes[1] & 0xe0) == 0xe0 &&
       (bytes[1] & 0x16) != 0x10))
    return MediaDecoderKind::Portable;
  if (startsWith(bytes, size, "OggS", 4) ||
      startsWith(bytes, size, "#!AMR\n", 6) ||
      startsWith(bytes, size, "#!AMR-WB\n", 9) ||
      (size >= 2 && bytes[0] == 0xff && (bytes[1] & 0xf6) == 0xf0) ||
      (size >= 12 && std::memcmp(bytes + 4, "ftyp", 4) == 0))
    return MediaDecoderKind::Ffmpeg;
  const MediaDecoderKind mime = fromMime(mime_type);
  return mime != MediaDecoderKind::Unknown ? mime : fromHint(locator_hint);
}

} // namespace oos::media
