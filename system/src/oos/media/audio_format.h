#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace oos::media {

enum class DecoderKind : uint8_t {
  BuiltIn,
  Platform,
};

struct AudioFormatSupport {
  const char *mime_type;
  const char *extensions;
  DecoderKind decoder;
  bool streaming;
  bool seekable;
};

// Returns only formats that this OOS build can actually decode. MIME types are
// the stable application-facing identifiers; extensions are discovery hints.
const std::vector<AudioFormatSupport> &supportedAudioFormats();

const AudioFormatSupport *findAudioFormat(std::string_view mime_type);

} // namespace oos::media
