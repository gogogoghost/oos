#include "oos/media/audio_format.h"

#include <algorithm>

namespace oos::media {

const std::vector<AudioFormatSupport> &supportedAudioFormats() {
  static const std::vector<AudioFormatSupport> formats = {
      {"audio/wav", "wav,wave", DecoderKind::BuiltIn, true, true},
      {"audio/mpeg", "mp3", DecoderKind::BuiltIn, true, true},
      {"audio/flac", "flac", DecoderKind::BuiltIn, true, true},
      {"audio/midi", "mid,midi,kar", DecoderKind::BuiltIn, true, true},
      {"audio/sp-midi", "mid", DecoderKind::BuiltIn, true, true},
      {"audio/mobile-xmf", "xmf,mxmf", DecoderKind::BuiltIn, true, true},
      {"audio/imelody", "imy", DecoderKind::BuiltIn, true, true},
      {"audio/rtttl", "rtttl", DecoderKind::BuiltIn, true, true},
      {"audio/vnd.nokia.ringing-tone", "ota", DecoderKind::BuiltIn, true, true},
      {"audio/aac", "aac", DecoderKind::BuiltIn, true, true},
      {"audio/mp4", "m4a,mp4", DecoderKind::BuiltIn, true, true},
      {"audio/3gpp", "3gp,3g2", DecoderKind::BuiltIn, true, true},
      {"audio/amr", "amr", DecoderKind::BuiltIn, true, true},
      {"audio/amr-wb", "awb", DecoderKind::BuiltIn, true, true},
      {"audio/ogg; codecs=vorbis", "ogg,oga", DecoderKind::BuiltIn, true, true},
      {"audio/ogg; codecs=opus", "ogg,oga,opus", DecoderKind::BuiltIn, true,
       true},
      {"audio/opus", "opus", DecoderKind::BuiltIn, true, true},
  };
  return formats;
}

const AudioFormatSupport *findAudioFormat(std::string_view mime_type) {
  const auto &formats = supportedAudioFormats();
  const auto found =
      std::find_if(formats.begin(), formats.end(),
                   [mime_type](const AudioFormatSupport &item) {
                     return mime_type == item.mime_type ||
                            (mime_type == "audio/x-wav" &&
                             std::string_view(item.mime_type) == "audio/wav") ||
                            (mime_type == "audio/x-flac" &&
                             std::string_view(item.mime_type) == "audio/flac");
                   });
  return found == formats.end() ? nullptr : &*found;
}

} // namespace oos::media
