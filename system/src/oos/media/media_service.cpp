#include "oos/media/media_service.h"

#include "oos/device/service_provider.h"
#include "oos/media/audio_decoder.h"
#include "oos/media/ffmpeg_decoder.h"
#include "oos/media/midi_decoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <vector>

namespace oos::media {
namespace {

constexpr uint32_t kMaxPlayers = 8;
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

    void start() {
      worker = std::thread([this] { run(); });
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

    void run() {
      AudioDecoder decoder;
      FfmpegDecoder ffmpeg_decoder;
      MidiDecoder midi_decoder;
      const bool midi = isMidiOrRingtonePath(path);
      const bool ffmpeg = !midi && isFfmpegAudioPath(path);
      const bool opened = midi     ? midi_decoder.openFile(path)
                          : ffmpeg ? ffmpeg_decoder.openFile(path)
                                   : decoder.openFile(path);
      if (!opened) {
        fail(midi     ? midi_decoder.lastError()
             : ffmpeg ? ffmpeg_decoder.lastError()
                      : decoder.lastError());
        return;
      }
      const DecodedAudioFormat format = midi     ? midi_decoder.format()
                                        : ffmpeg ? ffmpeg_decoder.format()
                                                 : decoder.format();
      if (format.channels > 2) {
        fail("audio output supports at most two channels");
        return;
      }
      hardware::PcmOutputConfig config = {static_cast<int>(format.sample_rate),
                                          static_cast<int>(format.channels),
                                          4096, usage};
      hardware::AudioStreamInfo stream_info;
      uint32_t opened_pcm = 0;
      if (!owner.services.pcmOpen(config, opened_pcm, stream_info)) {
        fail(owner.services.lastError());
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
          condition.wait_for(lock, std::chrono::milliseconds(4), [this] {
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
                                      : decode_error);
            break;
          }
          submitted_frames = seek_frame;
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
            fail(midi     ? midi_decoder.lastError()
                 : ffmpeg ? ffmpeg_decoder.lastError()
                          : decoder.lastError());
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
              fail("loop audio seek failed");
              break;
            }
            submitted_frames = 0;
            continue;
          }
        }
        int64_t accepted = 0;
        const int16_t *source = pcm.data() + buffer_offset * format.channels;
        if (!owner.services.pcmWrite(pcm_handle, source,
                                     buffered_frames - buffer_offset,
                                     accepted)) {
          fail(owner.services.lastError());
          break;
        }
        if (accepted == 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
          continue;
        }
        buffer_offset += static_cast<uint64_t>(accepted);
        std::lock_guard<std::mutex> lock(mutex);
        submitted_frames += static_cast<uint64_t>(accepted);
      }
      if (pcm_handle) {
        owner.services.pcmClose(pcm_handle);
        pcm_handle = 0;
      }
    }

    void fail(std::string message) {
      std::lock_guard<std::mutex> lock(mutex);
      error = std::move(message);
      state = PlayerState::Failed;
      want_playing = false;
    }

    Impl &owner;
    std::string path;
    hardware::AudioUsage usage = hardware::AudioUsage::Media;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable condition;
    PlayerState state = PlayerState::Preparing;
    std::string error;
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
      return nullptr;
    }
    return players[handle - 1].get();
  }

  device::ServiceProvider &services;
  std::string asset_directory;
  std::array<std::unique_ptr<Player>, kMaxPlayers> players;
  std::atomic<bool> focused{true};
  std::string error;
};

MediaService::MediaService(device::ServiceProvider &services,
                           std::string asset_directory)
    : impl_(std::make_unique<Impl>(services, std::move(asset_directory))) {}

MediaService::~MediaService() { closeAll(); }

bool MediaService::openAsset(const std::string &path,
                             hardware::AudioUsage usage, uint32_t &handle) {
  handle = 0;
  impl_->error.clear();
  if (!validRelativePath(path) || impl_->asset_directory.empty()) {
    impl_->error = "media asset path is invalid";
    return false;
  }
  const std::string absolute = impl_->asset_directory + "/" + path;
  struct stat status{};
  if (::stat(absolute.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
    impl_->error = "media asset is not a regular packaged file";
    return false;
  }
  for (size_t index = 0; index < impl_->players.size(); ++index) {
    if (impl_->players[index])
      continue;
    auto player = std::make_unique<Impl::Player>(*impl_);
    player->path = absolute;
    player->usage = usage;
    player->start();
    impl_->players[index] = std::move(player);
    handle = static_cast<uint32_t>(index + 1);
    return true;
  }
  impl_->error = "media player limit reached";
  return false;
}

bool MediaService::play(uint32_t handle) {
  Impl::Player *player = impl_->player(handle);
  if (!player)
    return false;
  std::lock_guard<std::mutex> lock(player->mutex);
  player->want_playing = true;
  player->condition.notify_all();
  return true;
}

bool MediaService::pause(uint32_t handle) {
  Impl::Player *player = impl_->player(handle);
  if (!player)
    return false;
  std::lock_guard<std::mutex> lock(player->mutex);
  player->want_playing = false;
  player->condition.notify_all();
  return true;
}

bool MediaService::seek(uint32_t handle, uint64_t position_ms) {
  Impl::Player *player = impl_->player(handle);
  if (!player)
    return false;
  std::lock_guard<std::mutex> lock(player->mutex);
  if (!player->sample_rate) {
    impl_->error = "media player is still preparing";
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
  Impl::Player *player = impl_->player(handle);
  if (!player || volume < 0.0F || volume > 1.0F)
    return false;
  uint32_t pcm_handle = 0;
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    player->volume = volume;
    pcm_handle = player->pcm_handle;
  }
  return !pcm_handle || impl_->services.pcmSetVolume(pcm_handle, volume);
}

bool MediaService::setLooping(uint32_t handle, bool looping) {
  Impl::Player *player = impl_->player(handle);
  if (!player)
    return false;
  std::lock_guard<std::mutex> lock(player->mutex);
  player->looping = looping;
  return true;
}

bool MediaService::status(uint32_t handle, PlayerStatus &status) {
  Impl::Player *player = impl_->player(handle);
  if (!player)
    return false;
  PlayerState state;
  uint32_t pcm_handle = 0;
  uint32_t sample_rate = 0;
  uint64_t duration_frames = 0;
  uint64_t submitted_frames = 0;
  std::string player_error;
  {
    std::lock_guard<std::mutex> lock(player->mutex);
    state = player->state;
    pcm_handle = player->pcm_handle;
    sample_rate = player->sample_rate;
    duration_frames = player->duration_frames;
    submitted_frames = player->submitted_frames;
    player_error = player->error;
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
            has_pcm ? pcm.underruns : 0};
  if (state == PlayerState::Failed)
    impl_->error = std::move(player_error);
  return true;
}

bool MediaService::close(uint32_t handle) {
  if (!impl_->player(handle))
    return false;
  impl_->players[handle - 1].reset();
  impl_->error.clear();
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
}

const std::string &MediaService::lastError() const { return impl_->error; }

} // namespace oos::media
