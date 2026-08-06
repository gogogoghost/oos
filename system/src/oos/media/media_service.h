#pragma once

#include "oos/hardware/audio_manager.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oos::device {
class ServiceProvider;
}

namespace oos::media {

enum class PlayerState : uint8_t {
  Preparing,
  Ready,
  Playing,
  Paused,
  Ended,
  Failed,
};

enum class MediaFailure : uint8_t {
  None,
  UnsupportedFormat,
  MalformedData,
  ResourceExhaustion,
  Io,
  Decoder,
};

struct MediaSourceLimits {
  uint64_t maximum_source_bytes = 0;
  uint64_t maximum_session_bytes = 0;
  uint32_t maximum_sources = 0;
  uint32_t maximum_players = 0;
};

struct PlayerStatus {
  PlayerState state = PlayerState::Failed;
  uint64_t position_ms = 0;
  uint64_t duration_ms = 0;
  int32_t underruns = 0;
  MediaFailure failure = MediaFailure::None;
};

// Session-owned asynchronous decoder/player service. Its worker threads never
// call guest code; destruction joins all workers before device services die.
class MediaService {
public:
  MediaService(device::ServiceProvider &services, std::string asset_directory);
  ~MediaService();

  MediaService(const MediaService &) = delete;
  MediaService &operator=(const MediaService &) = delete;

  bool openAsset(const std::string &path, hardware::AudioUsage usage,
                 uint32_t &handle);
  MediaSourceLimits sourceLimits() const;
  bool createSource(const uint8_t *bytes, size_t size,
                    const std::string &mime_type,
                    const std::string &locator_hint, uint32_t &handle);
  bool closeSource(uint32_t handle);
  bool openSource(uint32_t source, hardware::AudioUsage usage,
                  uint32_t &handle);
  bool play(uint32_t handle);
  bool pause(uint32_t handle);
  bool seek(uint32_t handle, uint64_t position_ms);
  bool setVolume(uint32_t handle, float volume);
  bool setLooping(uint32_t handle, bool looping);
  bool status(uint32_t handle, PlayerStatus &status);
  bool close(uint32_t handle);
  void setFocused(bool focused);
  void closeAll();

  const std::string &lastError() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::media
