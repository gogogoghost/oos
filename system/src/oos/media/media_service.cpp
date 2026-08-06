#include "oos/media/media_service.h"

#include "oos/device/service_provider.h"
#include "oos/media/audio_decoder.h"
#include "oos/media/ffmpeg_decoder.h"
#include "oos/media/media_source.h"
#include "oos/media/midi_decoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <new>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oos::media {
namespace {

constexpr uint32_t kMaxPlayers = 8;
constexpr uint32_t kMaxSources = 8;
constexpr uint64_t kMaxSourceBytes = 16ULL * 1024 * 1024;
constexpr uint64_t kMaxSessionBytes = 32ULL * 1024 * 1024;
constexpr uint32_t kDecodeFrames = 1024;

bool validRelativePath(const std::string &path) {
  if (path.empty() || path.size() > 1024 || path.front() == '/' ||
      path.back() == '/')
    return false;
  size_t begin = 0;
  while (begin < path.size()) {
    const size_t end = path.find('/', begin);
    const size_t length =
        (end == std::string::npos ? path.size() : end) - begin;
    if (length == 0 || (length == 1 && path[begin] == '.') ||
        (length == 2 && path[begin] == '.' && path[begin + 1] == '.'))
      return false;
    begin = end == std::string::npos ? path.size() : end + 1;
  }
  return path.find('\0') == std::string::npos;
}

} // namespace

class MediaService::Impl {
public:
  struct Player {
    explicit Player(Impl &owner) : owner(owner) {}

    ~Player() { stop(); }

    bool start() noexcept {
      try {
        worker = std::thread([this] { run(); });
        return true;
      } catch (const std::system_error &) {
        return false;
      } catch (const std::bad_alloc &) {
        return false;
      }
    }

    void stop() {
      {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
        condition.notify_all();
      }
      if (worker.joinable())
        worker.join();
    }

    void run() noexcept {
      try {
        runDecoder();
      } catch (const std::bad_alloc &) {
        fail("audio decoder allocation failed",
             MediaFailure::ResourceExhaustion);
      } catch (const std::exception &exception) {
        fail(std::string("audio decoder exception: ") + exception.what(),
             MediaFailure::Decoder);
      } catch (...) {
        fail("audio decoder failed with an unknown exception",
             MediaFailure::Decoder);
      }
      closePcm();
    }

    void closePcm() {
      uint32_t handle = 0;
      {
        std::lock_guard<std::mutex> lock(mutex);
        handle = std::exchange(pcm_handle, 0);
      }
      if (handle)
        owner.services.pcmClose(handle);
    }

    void runDecoder() {
      AudioDecoder decoder;
      FfmpegDecoder ffmpeg_decoder;
      MidiDecoder midi_decoder;
      const bool dynamic = source != nullptr;
      const MediaDecoderKind kind =
          dynamic ? source->decoder
                  : (isMidiOrRingtonePath(path) ? MediaDecoderKind::LegacyMidi
                     : isFfmpegAudioPath(path)  ? MediaDecoderKind::Ffmpeg
                                                : MediaDecoderKind::Portable);
      const bool midi = kind == MediaDecoderKind::StandardMidi ||
                        kind == MediaDecoderKind::LegacyMidi;
      const bool ffmpeg = kind == MediaDecoderKind::Ffmpeg;
      const bool opened =
          dynamic
              ? (midi ? midi_decoder.open(
                            source->bytes.data(), source->bytes.size(),
                            kind == MediaDecoderKind::StandardMidi)
                 : ffmpeg
                     ? ffmpeg_decoder.open(source->bytes.data(),
                                           source->bytes.size())
                     : decoder.open(source->bytes.data(), source->bytes.size()))
              : (midi     ? midi_decoder.openFile(path)
                 : ffmpeg ? ffmpeg_decoder.openFile(path)
                          : decoder.openFile(path));
      if (!opened) {
        const std::string message = midi     ? midi_decoder.lastError()
                                    : ffmpeg ? ffmpeg_decoder.lastError()
                                             : decoder.lastError();
        fail(message, dynamic ? MediaFailure::MalformedData : MediaFailure::Io);
        return;
      }
      const DecodedAudioFormat format = midi     ? midi_decoder.format()
                                        : ffmpeg ? ffmpeg_decoder.format()
                                                 : decoder.format();
      if (format.channels > 2) {
        fail("audio output supports at most two channels",
             MediaFailure::UnsupportedFormat);
        return;
      }
      hardware::PcmOutputConfig config = {static_cast<int>(format.sample_rate),
                                          static_cast<int>(format.channels),
                                          4096, usage};
      hardware::AudioStreamInfo stream_info;
      uint32_t opened_pcm = 0;
      if (!owner.services.pcmOpen(config, opened_pcm, stream_info)) {
        fail(owner.services.lastError(), MediaFailure::ResourceExhaustion);
        return;
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        pcm_handle = opened_pcm;
        owner.services.pcmSetVolume(pcm_handle, volume);
        sample_rate = format.sample_rate;
        duration_frames = midi     ? midi_decoder.lengthFrames()
                          : ffmpeg ? ffmpeg_decoder.lengthFrames()
                                   : decoder.lengthFrames();
        state = PlayerState::Ready;
        condition.notify_all();
      }

      std::vector<int16_t> pcm(static_cast<size_t>(kDecodeFrames) *
                               format.channels);
      uint64_t buffered_frames = 0;
      uint64_t buffer_offset = 0;
      while (true) {
        uint64_t seek_frame = 0;
        bool do_seek = false;
        {
          std::unique_lock<std::mutex> lock(mutex);
          condition.wait(lock, [this] {
            return stopping || seek_pending ||
                   (want_playing && owner.focused.load());
          });
          if (stopping)
            break;
          if (seek_pending) {
            seek_frame = requested_frame;
            seek_pending = false;
            do_seek = true;
          }
          if (!want_playing || !owner.focused.load()) {
            if (state == PlayerState::Playing) {
              owner.services.pcmPause(pcm_handle);
              state = PlayerState::Paused;
            }
            continue;
          }
          if (state != PlayerState::Playing) {
            owner.services.pcmResume(pcm_handle);
            state = PlayerState::Playing;
          }
        }
        if (do_seek) {
          const bool seeked = midi     ? midi_decoder.seek(seek_frame)
                              : ffmpeg ? ffmpeg_decoder.seek(seek_frame)
                                       : decoder.seek(seek_frame);
          if (!seeked || !owner.services.pcmFlush(pcm_handle)) {
            const std::string &decode_error = midi ? midi_decoder.lastError()
                                              : ffmpeg
                                                  ? ffmpeg_decoder.lastError()
                                                  : decoder.lastError();
            fail(decode_error.empty() ? owner.services.lastError()
                                      : decode_error,
                 MediaFailure::Decoder);
            break;
          }
          {
            std::lock_guard<std::mutex> lock(mutex);
            submitted_frames = seek_frame;
          }
          buffered_frames = 0;
          buffer_offset = 0;
        }
        if (buffer_offset == buffered_frames) {
          const bool decoded =
              midi ? midi_decoder.read(pcm.data(), kDecodeFrames,
                                       buffered_frames)
              : ffmpeg
                  ? ffmpeg_decoder.read(pcm.data(), kDecodeFrames,
                                        buffered_frames)
                  : decoder.read(pcm.data(), kDecodeFrames, buffered_frames);
          if (!decoded) {
            const std::string message = midi     ? midi_decoder.lastError()
                                        : ffmpeg ? ffmpeg_decoder.lastError()
                                                 : decoder.lastError();
            fail(message, MediaFailure::Decoder);
            break;
          }
          buffer_offset = 0;
          if (buffered_frames == 0) {
            bool repeat = false;
            {
              std::lock_guard<std::mutex> lock(mutex);
              repeat = looping;
              if (!repeat) {
                want_playing = false;
                state = PlayerState::Ended;
              }
            }
            if (!repeat)
              continue;
            const bool rewound = midi     ? midi_decoder.seek(0)
                                 : ffmpeg ? ffmpeg_decoder.seek(0)
                                          : decoder.seek(0);
            if (!rewound || !owner.services.pcmFlush(pcm_handle)) {
              fail("loop audio seek failed", MediaFailure::Decoder);
              break;
            }
            {
              std::lock_guard<std::mutex> lock(mutex);
              submitted_frames = 0;
            }
            continue;
          }
        }
        int64_t accepted = 0;
        const int16_t *source = pcm.data() + buffer_offset * format.channels;
        if (!owner.services.pcmWrite(pcm_handle, source,
                                     buffered_frames - buffer_offset,
                                     accepted)) {
          fail(owner.services.lastError(), MediaFailure::Io);
          break;
        }
        if (accepted == 0) {
          (void)owner.services.pcmWaitWritable(pcm_handle, 50);
          continue;
        }
        buffer_offset += static_cast<uint64_t>(accepted);
        std::lock_guard<std::mutex> lock(mutex);
        submitted_frames += static_cast<uint64_t>(accepted);
      }
      closePcm();
    }

    void fail(std::string message, MediaFailure failure_code) {
      std::lock_guard<std::mutex> lock(mutex);
      error = std::move(message);
      state = PlayerState::Failed;
      failure = failure_code;
      want_playing = false;
    }

    Impl &owner;
    std::string path;
    std::shared_ptr<const EncodedMediaSource> source;
    hardware::AudioUsage usage = hardware::AudioUsage::Media;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable condition;
    PlayerState state = PlayerState::Preparing;
    std::string error;
    MediaFailure failure = MediaFailure::None;
    uint32_t pcm_handle = 0;
    uint32_t sample_rate = 0;
    uint64_t duration_frames = 0;
    uint64_t submitted_frames = 0;
    uint64_t requested_frame = 0;
    float volume = 1.0F;
    bool want_playing = false;
    bool looping = false;
    bool seek_pending = false;
    bool stopping = false;
  };

  Impl(device::ServiceProvider &services, std::string asset_directory)
      : services(services), asset_directory(std::move(asset_directory)) {}

  Player *player(uint32_t handle) {
    if (handle == 0 || handle > players.size() || !players[handle - 1]) {
      error = "media player handle is invalid";
      error_code = MediaError::InvalidArgument;
      return nullptr;
    }
    return players[handle - 1].get();
  }

  std::shared_ptr<const EncodedMediaSource> source(uint32_t handle) {
    if (handle == 0 || handle > sources.size() || !sources[handle - 1]) {
      error = "media source handle is invalid";
      error_code = MediaError::InvalidArgument;
      return {};
    }
    return sources[handle - 1];
  }

  bool insertPlayer(std::unique_ptr<Player> value, uint32_t &handle) {
    for (size_t index = 0; index < players.size(); ++index) {
      if (players[index])
        continue;
      if (!value->start()) {
        error = "media decoder thread allocation failed";
        error_code = MediaError::ResourceExhaustion;
        return false;
      }
      players[index] = std::move(value);
      handle = static_cast<uint32_t>(index + 1);
      return true;
    }
    error = "media player limit reached";
    error_code = MediaError::ResourceExhaustion;
    return false;
  }

  uint64_t residentSourceBytes() const {
    std::unordered_set<const EncodedMediaSource *> seen;
    uint64_t total = 0;
    for (const auto &source : sources) {
      if (source && seen.insert(source.get()).second)
        total += source->bytes.size();
    }
    for (const auto &player : players) {
      if (player && player->source && seen.insert(player->source.get()).second)
        total += player->source->bytes.size();
    }
    return total;
  }

  device::ServiceProvider &services;
  std::string asset_directory;
  std::array<std::unique_ptr<Player>, kMaxPlayers> players;
  std::array<std::shared_ptr<EncodedMediaSource>, kMaxSources> sources;
  std::atomic<bool> focused{true};
  std::string error;
  MediaError error_code = MediaError::None;
};

MediaService::MediaService(device::ServiceProvider &services,
                           std::string asset_directory)
    : impl_(std::make_unique<Impl>(services, std::move(asset_directory))) {}

MediaService::~MediaService() { closeAll(); }

bool MediaService::openAsset(const std::string &path,
                             hardware::AudioUsage usage, uint32_t &handle) {
  handle = 0;
  impl_->error.clear();
  impl_->error_code = MediaError::None;
  if (!validRelativePath(path) || impl_->asset_directory.empty()) {
    impl_->error = "media asset path is invalid";
    impl_->error_code = MediaError::InvalidArgument;
    return false;
  }
  const std::string absolute = impl_->asset_directory + "/" + path;
  struct stat status{};
  if (::stat(absolute.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
    impl_->error = "media asset is not a regular packaged file";
    impl_->error_code = MediaError::Io;
    return false;
  }
  try {
    auto player = std::make_unique<Impl::Player>(*impl_);
    player->path = absolute;
    player->usage = usage;
    return impl_->insertPlayer(std::move(player), handle);
  } catch (const std::bad_alloc &) {
    impl_->error = "media player allocation failed";
    impl_->error_code = MediaError::ResourceExhaustion;
    return false;
  }
}

MediaSourceLimits MediaService::sourceLimits() const {
  return {kMaxSourceBytes, kMaxSessionBytes, kMaxSources, kMaxPlayers};
}

bool MediaService::createSource(const uint8_t *bytes, size_t size,
                                const std::string &mime_type,
                                const std::string &locator_hint,
                                uint32_t &handle) {
  handle = 0;
  impl_->error.clear();
  impl_->error_code = MediaError::None;
  if (!bytes || size == 0) {
    impl_->error = "encoded media source is empty";
    impl_->error_code = MediaError::InvalidArgument;
    return false;
  }
  try {
    if (size > kMaxSourceBytes ||
        size > kMaxSessionBytes - impl_->residentSourceBytes()) {
      impl_->error = "encoded media source exceeds session limits";
      impl_->error_code = MediaError::ResourceExhaustion;
      return false;
    }
  } catch (const std::bad_alloc &) {
    impl_->error = "encoded media source exceeds session limits";
    impl_->error_code = MediaError::ResourceExhaustion;
    return false;
  }
  const MediaDecoderKind decoder =
      identifyMedia(bytes, size, mime_type, locator_hint);
  if (decoder == MediaDecoderKind::Unknown) {
    impl_->error = "encoded media format is unsupported";
    impl_->error_code = MediaError::UnsupportedFormat;
    return false;
  }
  for (size_t index = 0; index < impl_->sources.size(); ++index) {
    if (impl_->sources[index])
      continue;
    try {
      auto source = std::make_shared<EncodedMediaSource>();
      source->bytes.assign(bytes, bytes + size);
      source->mime_type = mime_type;
      source->locator_hint = locator_hint;
      source->decoder = decoder;
      impl_->sources[index] = std::move(source);
    } catch (const std::bad_alloc &) {
      impl_->error = "encoded media source allocation failed";
      impl_->error_code = MediaError::ResourceExhaustion;
      return false;
    }
    handle = static_cast<uint32_t>(index + 1);
    return true;
  }
  impl_->error = "media source limit reached";
  impl_->error_code = MediaError::ResourceExhaustion;
  return false;
}

bool MediaService::closeSource(uint32_t handle) {
  const auto source = impl_->source(handle);
  if (!source)
    return false;
  impl_->sources[handle - 1].reset();
  impl_->error.clear();
  impl_->error_code = MediaError::None;
  return true;
}

bool MediaService::openSource(uint32_t source_handle,
                              hardware::AudioUsage usage, uint32_t &handle) {
  handle = 0;
  impl_->error.clear();
  impl_->error_code = MediaError::None;
  const auto source = impl_->source(source_handle);
  if (!source)
    return false;
  try {
    auto player = std::make_unique<Impl::Player>(*impl_);
    player->source = source;
    player->usage = usage;
    return impl_->insertPlayer(std::move(player), handle);
  } catch (const std::bad_alloc &) {
    impl_->error = "media player allocation failed";
    impl_->error_code = MediaError::ResourceExhaustion;
    return false;
  }
}

bool MediaService::play(uint32_t handle) {
  impl_->error.clear();
  impl_->error_code = MediaError::None;
  Impl::Player *player = impl_->player(handle);
  if (!player)
    return false;
  std::lock_guard<std::mutex> lock(player->mutex);
  player->want_playing = true;
  player->condition.notify_all();
  return true;
}

bool MediaService::pause(uint32_t handle) {
  impl_->error.clear();
  impl_->error_code = MediaError::None;
  Impl::Player *player = impl_->player(handle);
  if (!player)
    return false;
  std::lock_guard<std::mutex> lock(player->mutex);
  player->want_playing = false;
  player->condition.notify_all();
  return true;
}

bool MediaService::seek(uint32_t handle, uint64_t position_ms) {
  impl_->error.clear();
  impl_->error_code = MediaError::None;
  Impl::Player *player = impl_->player(handle);
  if (!player)
    return false;
  std::lock_guard<std::mutex> lock(player->mutex);
  if (!player->sample_rate) {
    impl_->error = "media player is still preparing";
    impl_->error_code = MediaError::Busy;
    return false;
  }
  player->requested_frame = position_ms * player->sample_rate / 1000;
  if (player->duration_frames)
    player->requested_frame =
        std::min(player->requested_frame, player->duration_frames);
  player->seek_pending = true;
  player->condition.notify_all();
  return true;
}

bool MediaService::setVolume(uint32_t handle, float volume) {
  impl_->error.clear();
  impl_->error_code = MediaError::None;
  Impl::Player *player = impl_->player(handle);
  if (!player || volume < 0.0F || volume > 1.0F) {
    if (player)
      impl_->error_code = MediaError::InvalidArgument;
    return false;
  }
  uint32_t pcm_handle = 0;
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    player->volume = volume;
    pcm_handle = player->pcm_handle;
  }
  if (!pcm_handle || impl_->services.pcmSetVolume(pcm_handle, volume))
    return true;
  impl_->error = impl_->services.lastError();
  impl_->error_code = MediaError::Io;
  return false;
}

bool MediaService::setLooping(uint32_t handle, bool looping) {
  impl_->error.clear();
  impl_->error_code = MediaError::None;
  Impl::Player *player = impl_->player(handle);
  if (!player)
    return false;
  std::lock_guard<std::mutex> lock(player->mutex);
  player->looping = looping;
  player->condition.notify_all();
  return true;
}

bool MediaService::status(uint32_t handle, PlayerStatus &status) {
  impl_->error.clear();
  impl_->error_code = MediaError::None;
  Impl::Player *player = impl_->player(handle);
  if (!player)
    return false;
  PlayerState state;
  uint32_t pcm_handle = 0;
  uint32_t sample_rate = 0;
  uint64_t duration_frames = 0;
  uint64_t submitted_frames = 0;
  std::string player_error;
  MediaFailure failure = MediaFailure::None;
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    state = player->state;
    pcm_handle = player->pcm_handle;
    sample_rate = player->sample_rate;
    duration_frames = player->duration_frames;
    submitted_frames = player->submitted_frames;
    player_error = player->error;
    failure = player->failure;
  }
  hardware::PcmOutputStatus pcm;
  const bool has_pcm = pcm_handle && impl_->services.pcmStatus(pcm_handle, pcm);
  const uint64_t queued =
      has_pcm ? static_cast<uint64_t>(std::max<int64_t>(0, pcm.queued_frames))
              : 0;
  const uint64_t played_frames =
      submitted_frames > queued ? submitted_frames - queued : 0;
  status = {state, sample_rate ? played_frames * 1000 / sample_rate : 0,
            sample_rate ? duration_frames * 1000 / sample_rate : 0,
            has_pcm ? pcm.underruns : 0, failure};
  if (state == PlayerState::Failed) {
    impl_->error = std::move(player_error);
    switch (failure) {
    case MediaFailure::UnsupportedFormat:
      impl_->error_code = MediaError::UnsupportedFormat;
      break;
    case MediaFailure::MalformedData:
      impl_->error_code = MediaError::MalformedData;
      break;
    case MediaFailure::ResourceExhaustion:
      impl_->error_code = MediaError::ResourceExhaustion;
      break;
    case MediaFailure::Io:
      impl_->error_code = MediaError::Io;
      break;
    case MediaFailure::Decoder:
      impl_->error_code = MediaError::Decoder;
      break;
    case MediaFailure::None:
      impl_->error_code = MediaError::None;
      break;
    }
  }
  return true;
}

bool MediaService::close(uint32_t handle) {
  impl_->error.clear();
  impl_->error_code = MediaError::None;
  if (!impl_->player(handle))
    return false;
  impl_->players[handle - 1].reset();
  impl_->error.clear();
  impl_->error_code = MediaError::None;
  return true;
}

void MediaService::setFocused(bool focused) {
  impl_->focused = focused;
  for (const auto &player : impl_->players) {
    if (player)
      player->condition.notify_all();
  }
}

void MediaService::closeAll() {
  if (!impl_)
    return;
  for (auto &player : impl_->players)
    player.reset();
  for (auto &source : impl_->sources)
    source.reset();
}

const std::string &MediaService::lastError() const { return impl_->error; }

MediaError MediaService::lastErrorCode() const { return impl_->error_code; }

} // namespace oos::media
