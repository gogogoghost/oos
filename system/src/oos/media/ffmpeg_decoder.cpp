#include "oos/media/ffmpeg_decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <limits>
#include <vector>

namespace oos::media {
namespace {

std::string ffmpegError(const char *operation, int result) {
  char message[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(result, message, sizeof(message));
  return std::string(operation) + ": " + message;
}

} // namespace

class FfmpegDecoder::Impl {
public:
  static int readPacket(void *opaque, uint8_t *buffer, int size) {
    auto *self = static_cast<Impl *>(opaque);
    if (!self || size <= 0 || self->memory_offset >= self->memory_size)
      return AVERROR_EOF;
    const size_t count = std::min<size_t>(
        static_cast<size_t>(size), self->memory_size - self->memory_offset);
    std::memcpy(buffer, self->memory + self->memory_offset, count);
    self->memory_offset += count;
    return static_cast<int>(count);
  }

  static int64_t seekMemory(void *opaque, int64_t offset, int whence) {
    auto *self = static_cast<Impl *>(opaque);
    if (!self)
      return AVERROR(EINVAL);
    if (whence == AVSEEK_SIZE)
      return static_cast<int64_t>(self->memory_size);
    const int origin = whence & ~AVSEEK_FORCE;
    int64_t base = 0;
    if (origin == SEEK_CUR)
      base = static_cast<int64_t>(self->memory_offset);
    else if (origin == SEEK_END)
      base = static_cast<int64_t>(self->memory_size);
    else if (origin != SEEK_SET)
      return AVERROR(EINVAL);
    if (offset < -base ||
        offset > static_cast<int64_t>(self->memory_size) - base)
      return AVERROR(EINVAL);
    self->memory_offset = static_cast<size_t>(base + offset);
    return static_cast<int64_t>(self->memory_offset);
  }

  void close() {
    if (resampler)
      swr_free(&resampler);
    if (frame)
      av_frame_free(&frame);
    if (packet)
      av_packet_free(&packet);
    if (codec)
      avcodec_free_context(&codec);
    if (container)
      avformat_close_input(&container);
    if (io) {
      av_freep(&io->buffer);
      avio_context_free(&io);
    }
    memory = nullptr;
    memory_size = 0;
    memory_offset = 0;
    stream = -1;
    output = {};
    duration_frames = 0;
    pending.clear();
    pending_offset = 0;
    input_eof = false;
    decoder_eof = false;
  }

  bool receive() {
    while (true) {
      const int received = avcodec_receive_frame(codec, frame);
      if (received == 0) {
        const int maximum = swr_get_out_samples(resampler, frame->nb_samples);
        if (maximum <= 0) {
          error = "audio resampler returned an invalid capacity";
          return false;
        }
        pending.resize(static_cast<size_t>(maximum) * output.channels);
        uint8_t *destination = reinterpret_cast<uint8_t *>(pending.data());
        const int converted =
            swr_convert(resampler, &destination, maximum,
                        const_cast<const uint8_t **>(frame->extended_data),
                        frame->nb_samples);
        av_frame_unref(frame);
        if (converted < 0) {
          error = ffmpegError("convert decoded audio", converted);
          return false;
        }
        pending.resize(static_cast<size_t>(converted) * output.channels);
        pending_offset = 0;
        if (converted > 0)
          return true;
        continue;
      }
      if (received == AVERROR_EOF) {
        decoder_eof = true;
        return true;
      }
      if (received != AVERROR(EAGAIN)) {
        error = ffmpegError("receive decoded audio", received);
        return false;
      }
      if (input_eof) {
        const int sent = avcodec_send_packet(codec, nullptr);
        if (sent != 0 && sent != AVERROR_EOF && sent != AVERROR(EAGAIN)) {
          error = ffmpegError("flush audio decoder", sent);
          return false;
        }
        continue;
      }
      int read_result = 0;
      do {
        av_packet_unref(packet);
        read_result = av_read_frame(container, packet);
      } while (read_result >= 0 && packet->stream_index != stream);
      if (read_result == AVERROR_EOF) {
        input_eof = true;
        continue;
      }
      if (read_result < 0) {
        error = ffmpegError("read audio packet", read_result);
        return false;
      }
      const int sent = avcodec_send_packet(codec, packet);
      av_packet_unref(packet);
      if (sent != 0 && sent != AVERROR(EAGAIN)) {
        error = ffmpegError("submit audio packet", sent);
        return false;
      }
    }
  }

  AVFormatContext *container = nullptr;
  AVIOContext *io = nullptr;
  AVCodecContext *codec = nullptr;
  SwrContext *resampler = nullptr;
  AVPacket *packet = nullptr;
  AVFrame *frame = nullptr;
  int stream = -1;
  DecodedAudioFormat output;
  uint64_t duration_frames = 0;
  std::vector<int16_t> pending;
  size_t pending_offset = 0;
  bool input_eof = false;
  bool decoder_eof = false;
  std::string error;
  const uint8_t *memory = nullptr;
  size_t memory_size = 0;
  size_t memory_offset = 0;
};

FfmpegDecoder::FfmpegDecoder() : impl_(std::make_unique<Impl>()) {}

FfmpegDecoder::~FfmpegDecoder() { impl_->close(); }

bool FfmpegDecoder::openFile(const std::string &path) {
  impl_->close();
  impl_->error.clear();
  int result =
      avformat_open_input(&impl_->container, path.c_str(), nullptr, nullptr);
  if (result >= 0)
    result = avformat_find_stream_info(impl_->container, nullptr);
  if (result < 0) {
    impl_->error = ffmpegError("open audio container", result);
    impl_->close();
    return false;
  }
  const AVCodec *decoder = nullptr;
  impl_->stream = av_find_best_stream(impl_->container, AVMEDIA_TYPE_AUDIO, -1,
                                      -1, &decoder, 0);
  if (impl_->stream < 0 || !decoder) {
    impl_->error = "audio container has no supported audio stream";
    impl_->close();
    return false;
  }
  impl_->codec = avcodec_alloc_context3(decoder);
  AVStream *stream = impl_->container->streams[impl_->stream];
  if (!impl_->codec ||
      (result = avcodec_parameters_to_context(impl_->codec, stream->codecpar)) <
          0 ||
      (result = avcodec_open2(impl_->codec, decoder, nullptr)) < 0) {
    impl_->error = ffmpegError("open audio decoder", result);
    impl_->close();
    return false;
  }
  if (impl_->codec->sample_rate <= 0 ||
      impl_->codec->ch_layout.nb_channels <= 0) {
    impl_->error = "decoded audio format is invalid";
    impl_->close();
    return false;
  }
  AVChannelLayout output_layout;
  av_channel_layout_default(&output_layout,
                            std::min(2, impl_->codec->ch_layout.nb_channels));
  result = swr_alloc_set_opts2(
      &impl_->resampler, &output_layout, AV_SAMPLE_FMT_S16,
      impl_->codec->sample_rate, &impl_->codec->ch_layout,
      impl_->codec->sample_fmt, impl_->codec->sample_rate, 0, nullptr);
  av_channel_layout_uninit(&output_layout);
  if (result < 0 || (result = swr_init(impl_->resampler)) < 0) {
    impl_->error = ffmpegError("initialize audio resampler", result);
    impl_->close();
    return false;
  }
  impl_->packet = av_packet_alloc();
  impl_->frame = av_frame_alloc();
  if (!impl_->packet || !impl_->frame) {
    impl_->error = "allocate audio decoder buffers failed";
    impl_->close();
    return false;
  }
  impl_->output = {
      static_cast<uint32_t>(impl_->codec->sample_rate),
      static_cast<uint32_t>(std::min(2, impl_->codec->ch_layout.nb_channels))};
  if (stream->duration != AV_NOPTS_VALUE) {
    impl_->duration_frames =
        av_rescale_q(stream->duration, stream->time_base,
                     AVRational{1, impl_->codec->sample_rate});
  } else if (impl_->container->duration != AV_NOPTS_VALUE) {
    impl_->duration_frames =
        av_rescale_q(impl_->container->duration, AV_TIME_BASE_Q,
                     AVRational{1, impl_->codec->sample_rate});
  }
  return true;
}

bool FfmpegDecoder::open(const uint8_t *encoded, size_t encoded_bytes) {
  impl_->close();
  impl_->error.clear();
  if (!encoded || encoded_bytes == 0 ||
      encoded_bytes >
          static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
    impl_->error = "encoded FFmpeg audio is empty or too large";
    return false;
  }
  impl_->memory = encoded;
  impl_->memory_size = encoded_bytes;
  constexpr int kIoBufferBytes = 4096;
  uint8_t *buffer = static_cast<uint8_t *>(av_malloc(kIoBufferBytes));
  if (!buffer) {
    impl_->error = "allocate in-memory audio I/O buffer failed";
    impl_->close();
    return false;
  }
  impl_->io = avio_alloc_context(buffer, kIoBufferBytes, 0, impl_.get(),
                                 Impl::readPacket, nullptr, Impl::seekMemory);
  impl_->container = avformat_alloc_context();
  if (!impl_->io || !impl_->container) {
    if (!impl_->io)
      av_free(buffer);
    impl_->error = "allocate in-memory audio container failed";
    impl_->close();
    return false;
  }
  impl_->container->pb = impl_->io;
  impl_->container->flags |= AVFMT_FLAG_CUSTOM_IO;
  int result =
      avformat_open_input(&impl_->container, nullptr, nullptr, nullptr);
  if (result >= 0)
    result = avformat_find_stream_info(impl_->container, nullptr);
  if (result < 0) {
    impl_->error = ffmpegError("open in-memory audio container", result);
    impl_->close();
    return false;
  }
  const AVCodec *decoder = nullptr;
  impl_->stream = av_find_best_stream(impl_->container, AVMEDIA_TYPE_AUDIO, -1,
                                      -1, &decoder, 0);
  if (impl_->stream < 0 || !decoder) {
    impl_->error = "in-memory audio has no supported stream";
    impl_->close();
    return false;
  }
  impl_->codec = avcodec_alloc_context3(decoder);
  AVStream *stream = impl_->container->streams[impl_->stream];
  if (!impl_->codec ||
      (result = avcodec_parameters_to_context(impl_->codec, stream->codecpar)) <
          0 ||
      (result = avcodec_open2(impl_->codec, decoder, nullptr)) < 0) {
    impl_->error = ffmpegError("open in-memory audio decoder", result);
    impl_->close();
    return false;
  }
  if (impl_->codec->sample_rate <= 0 ||
      impl_->codec->ch_layout.nb_channels <= 0) {
    impl_->error = "decoded in-memory audio format is invalid";
    impl_->close();
    return false;
  }
  AVChannelLayout output_layout;
  av_channel_layout_default(&output_layout,
                            std::min(2, impl_->codec->ch_layout.nb_channels));
  result = swr_alloc_set_opts2(
      &impl_->resampler, &output_layout, AV_SAMPLE_FMT_S16,
      impl_->codec->sample_rate, &impl_->codec->ch_layout,
      impl_->codec->sample_fmt, impl_->codec->sample_rate, 0, nullptr);
  av_channel_layout_uninit(&output_layout);
  if (result < 0 || (result = swr_init(impl_->resampler)) < 0) {
    impl_->error = ffmpegError("initialize in-memory audio resampler", result);
    impl_->close();
    return false;
  }
  impl_->packet = av_packet_alloc();
  impl_->frame = av_frame_alloc();
  if (!impl_->packet || !impl_->frame) {
    impl_->error = "allocate in-memory audio decoder buffers failed";
    impl_->close();
    return false;
  }
  impl_->output = {
      static_cast<uint32_t>(impl_->codec->sample_rate),
      static_cast<uint32_t>(std::min(2, impl_->codec->ch_layout.nb_channels))};
  if (stream->duration != AV_NOPTS_VALUE)
    impl_->duration_frames =
        av_rescale_q(stream->duration, stream->time_base,
                     AVRational{1, impl_->codec->sample_rate});
  else if (impl_->container->duration != AV_NOPTS_VALUE)
    impl_->duration_frames =
        av_rescale_q(impl_->container->duration, AV_TIME_BASE_Q,
                     AVRational{1, impl_->codec->sample_rate});
  return true;
}

void FfmpegDecoder::close() { impl_->close(); }

bool FfmpegDecoder::read(int16_t *samples, uint64_t frame_capacity,
                         uint64_t &frames_read) {
  frames_read = 0;
  if (!impl_->codec || (!samples && frame_capacity)) {
    impl_->error = "FFmpeg decoder is not open or output is invalid";
    return false;
  }
  if (frame_capacity >
      std::numeric_limits<size_t>::max() / impl_->output.channels) {
    impl_->error = "FFmpeg decoder read is too large";
    return false;
  }
  while (frames_read < frame_capacity) {
    const size_t pending_frames =
        impl_->pending.size() / impl_->output.channels - impl_->pending_offset;
    if (pending_frames) {
      const size_t copy_frames = static_cast<size_t>(
          std::min<uint64_t>(pending_frames, frame_capacity - frames_read));
      std::memcpy(samples + frames_read * impl_->output.channels,
                  impl_->pending.data() +
                      impl_->pending_offset * impl_->output.channels,
                  copy_frames * impl_->output.channels * sizeof(int16_t));
      impl_->pending_offset += copy_frames;
      frames_read += copy_frames;
      continue;
    }
    impl_->pending.clear();
    impl_->pending_offset = 0;
    if (impl_->decoder_eof)
      break;
    if (!impl_->receive())
      return false;
  }
  return true;
}

bool FfmpegDecoder::seek(uint64_t frame) {
  if (!impl_->codec || impl_->stream < 0) {
    impl_->error = "FFmpeg decoder is not open";
    return false;
  }
  AVStream *stream = impl_->container->streams[impl_->stream];
  const int64_t timestamp = av_rescale_q(
      std::min<uint64_t>(frame, std::numeric_limits<int64_t>::max()),
      AVRational{1, impl_->codec->sample_rate}, stream->time_base);
  const int result = av_seek_frame(impl_->container, impl_->stream, timestamp,
                                   AVSEEK_FLAG_BACKWARD);
  if (result < 0) {
    impl_->error = ffmpegError("seek audio stream", result);
    return false;
  }
  avcodec_flush_buffers(impl_->codec);
  swr_close(impl_->resampler);
  if (swr_init(impl_->resampler) < 0) {
    impl_->error = "reset audio resampler failed";
    return false;
  }
  impl_->pending.clear();
  impl_->pending_offset = 0;
  impl_->input_eof = false;
  impl_->decoder_eof = false;
  return true;
}

uint64_t FfmpegDecoder::lengthFrames() const { return impl_->duration_frames; }

const DecodedAudioFormat &FfmpegDecoder::format() const {
  return impl_->output;
}

const std::string &FfmpegDecoder::lastError() const { return impl_->error; }

bool FfmpegDecoder::opened() const { return impl_->codec != nullptr; }

bool isFfmpegAudioPath(const std::string &path) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos)
    return false;
  std::string extension = path.substr(dot + 1);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char value) {
                   return static_cast<char>(std::tolower(value));
                 });
  return extension == "aac" || extension == "m4a" || extension == "mp4" ||
         extension == "3gp" || extension == "3g2" || extension == "amr" ||
         extension == "awb" || extension == "ogg" || extension == "oga" ||
         extension == "opus";
}

} // namespace oos::media
