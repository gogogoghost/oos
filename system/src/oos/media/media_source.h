#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oos::media {

enum class MediaDecoderKind : uint8_t {
  Unknown,
  Portable,
  Ffmpeg,
  StandardMidi,
  LegacyMidi,
};

struct EncodedMediaSource {
  std::vector<uint8_t> bytes;
  std::string mime_type;
  std::string locator_hint;
  MediaDecoderKind decoder = MediaDecoderKind::Unknown;
};

MediaDecoderKind identifyMedia(const uint8_t *bytes, size_t size,
                               const std::string &mime_type,
                               const std::string &locator_hint);

} // namespace oos::media
