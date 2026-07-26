#include "oos/hardware/codec_manager.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace oos::hardware {
namespace {

constexpr const char *kH264Mime = "video/avc";
constexpr int32_t kColorFormatYuv420SemiPlanar = 21;

std::string mediaError(const char *operation, media_status_t status) {
  return std::string(operation) + " failed (status=" + std::to_string(status) +
         ")";
}

bool isHardwareComponent(const std::string &name) {
  return name.rfind("OMX.qcom.", 0) == 0 || name.rfind("c2.qti.", 0) == 0;
}

class CodecResources {
public:
  ~CodecResources() {
    if (started)
      AMediaCodec_stop(codec);
    if (codec)
      AMediaCodec_delete(codec);
    if (format)
      AMediaFormat_delete(format);
  }

  AMediaCodec *codec = nullptr;
  AMediaFormat *format = nullptr;
  bool started = false;
};

struct EncodedPacket {
  std::vector<uint8_t> bytes;
  int64_t presentation_time_us = 0;
  uint32_t flags = 0;
};

void fillNv12Frame(uint8_t *data, size_t capacity, int width, int height,
                   int frame_number) {
  const size_t luminance_size = static_cast<size_t>(width) * height;
  const size_t frame_size = luminance_size * 3 / 2;
  if (capacity < frame_size)
    return;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      data[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>(
          32 + ((x + frame_number * 12) * 190 / std::max(width, 1)) % 190);
    }
  }
  std::memset(data + luminance_size, 128, frame_size - luminance_size);
}

bool decodeH264(const std::vector<EncodedPacket> &packets, int width,
                int height, int expected_frames, int timeout_ms,
                CodecResult &result, std::string &error) {
  CodecResources resources;
  resources.codec = AMediaCodec_createDecoderByType(kH264Mime);
  if (!resources.codec) {
    error = "no H.264 decoder is available";
    return false;
  }
  char *component_name = nullptr;
  if (AMediaCodec_getName(resources.codec, &component_name) == AMEDIA_OK &&
      component_name) {
    result.decoder_name = component_name;
    AMediaCodec_releaseName(resources.codec, component_name);
  } else {
    result.decoder_name = "unknown";
  }

  resources.format = AMediaFormat_new();
  if (!resources.format) {
    error = "create decoder format failed";
    return false;
  }
  AMediaFormat_setString(resources.format, AMEDIAFORMAT_KEY_MIME, kH264Mime);
  AMediaFormat_setInt32(resources.format, AMEDIAFORMAT_KEY_WIDTH, width);
  AMediaFormat_setInt32(resources.format, AMEDIAFORMAT_KEY_HEIGHT, height);
  AMediaFormat_setInt32(resources.format, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                        kColorFormatYuv420SemiPlanar);
  for (const EncodedPacket &packet : packets) {
    if ((packet.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0 &&
        !packet.bytes.empty()) {
      AMediaFormat_setBuffer(resources.format, "csd-0", packet.bytes.data(),
                             packet.bytes.size());
      break;
    }
  }
  media_status_t status = AMediaCodec_configure(
      resources.codec, resources.format, nullptr, nullptr, 0);
  if (status != AMEDIA_OK) {
    error = mediaError("configure H.264 decoder", status);
    return false;
  }
  status = AMediaCodec_start(resources.codec);
  if (status != AMEDIA_OK) {
    error = mediaError("start H.264 decoder", status);
    return false;
  }
  resources.started = true;

  size_t packet_index = 0;
  bool eos_queued = false;
  bool eos_received = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!eos_received && std::chrono::steady_clock::now() < deadline) {
    if (!eos_queued) {
      const ssize_t input =
          AMediaCodec_dequeueInputBuffer(resources.codec, 10000);
      if (input >= 0) {
        while (packet_index < packets.size() &&
               (packets[packet_index].flags &
                AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0)
          ++packet_index;
        const bool eos = packet_index == packets.size();
        size_t capacity = 0;
        uint8_t *buffer = AMediaCodec_getInputBuffer(
            resources.codec, static_cast<size_t>(input), &capacity);
        const size_t byte_count = eos ? 0 : packets[packet_index].bytes.size();
        if (!buffer || capacity < byte_count) {
          error = "H.264 decoder input buffer is too small";
          return false;
        }
        if (!eos)
          std::memcpy(buffer, packets[packet_index].bytes.data(), byte_count);
        const int64_t presentation_time =
            eos ? static_cast<int64_t>(expected_frames) * 1000000LL / 15LL
                : packets[packet_index].presentation_time_us;
        const uint32_t flags =
            eos ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM
                : packets[packet_index].flags &
                      ~static_cast<uint32_t>(
                          AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG);
        status = AMediaCodec_queueInputBuffer(
            resources.codec, static_cast<size_t>(input), 0, byte_count,
            presentation_time, flags);
        if (status != AMEDIA_OK) {
          error = mediaError("queue H.264 decoder input", status);
          return false;
        }
        if (eos)
          eos_queued = true;
        else
          ++packet_index;
      }
    }

    AMediaCodecBufferInfo info{};
    const ssize_t output =
        AMediaCodec_dequeueOutputBuffer(resources.codec, &info, 10000);
    if (output >= 0) {
      if (info.size > 0 &&
          (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) == 0)
        ++result.decoded_frames;
      eos_received = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
      status = AMediaCodec_releaseOutputBuffer(
          resources.codec, static_cast<size_t>(output), false);
      if (status != AMEDIA_OK) {
        error = mediaError("release H.264 decoder output", status);
        return false;
      }
    }
  }
  if (!eos_received || result.decoded_frames == 0) {
    error = eos_received ? "H.264 decoder produced no frames"
                         : "H.264 decoder did not reach EOS before timeout";
    return false;
  }
  result.decoder_hardware_accelerated =
      isHardwareComponent(result.decoder_name);
  return true;
}

} // namespace

bool CodecManager::testH264RoundTrip(int width, int height, int frame_count,
                                     CodecResult &result, int timeout_ms) {
  result = {};
  error_.clear();
  if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0 ||
      frame_count <= 0 || timeout_ms <= 0) {
    error_ = "invalid H.264 encoder test parameters";
    return false;
  }

  CodecResources resources;
  resources.codec = AMediaCodec_createEncoderByType(kH264Mime);
  if (!resources.codec) {
    error_ = "no H.264 encoder is available";
    return false;
  }
  char *component_name = nullptr;
  const media_status_t name_status =
      AMediaCodec_getName(resources.codec, &component_name);
  if (name_status == AMEDIA_OK && component_name) {
    result.encoder_name = component_name;
    AMediaCodec_releaseName(resources.codec, component_name);
  } else {
    result.encoder_name = "unknown";
  }

  resources.format = AMediaFormat_new();
  if (!resources.format) {
    error_ = "create encoder format failed";
    return false;
  }
  AMediaFormat_setString(resources.format, AMEDIAFORMAT_KEY_MIME, kH264Mime);
  AMediaFormat_setInt32(resources.format, AMEDIAFORMAT_KEY_WIDTH, width);
  AMediaFormat_setInt32(resources.format, AMEDIAFORMAT_KEY_HEIGHT, height);
  AMediaFormat_setInt32(resources.format, AMEDIAFORMAT_KEY_BIT_RATE, 400000);
  AMediaFormat_setInt32(resources.format, AMEDIAFORMAT_KEY_FRAME_RATE, 15);
  AMediaFormat_setInt32(resources.format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
  AMediaFormat_setInt32(resources.format, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                        kColorFormatYuv420SemiPlanar);

  media_status_t status =
      AMediaCodec_configure(resources.codec, resources.format, nullptr, nullptr,
                            AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
  if (status != AMEDIA_OK) {
    error_ = mediaError("configure H.264 encoder", status);
    return false;
  }
  status = AMediaCodec_start(resources.codec);
  if (status != AMEDIA_OK) {
    error_ = mediaError("start H.264 encoder", status);
    return false;
  }
  resources.started = true;

  const size_t frame_size = static_cast<size_t>(width) * height * 3 / 2;
  int queued_frames = 0;
  bool eos_queued = false;
  bool eos_received = false;
  std::vector<EncodedPacket> packets;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!eos_received && std::chrono::steady_clock::now() < deadline) {
    if (!eos_queued) {
      const ssize_t input =
          AMediaCodec_dequeueInputBuffer(resources.codec, 10000);
      if (input >= 0) {
        size_t capacity = 0;
        uint8_t *buffer = AMediaCodec_getInputBuffer(
            resources.codec, static_cast<size_t>(input), &capacity);
        if (!buffer || capacity < frame_size) {
          error_ = "H.264 encoder input buffer is smaller than one NV12 frame";
          return false;
        }
        const bool eos = queued_frames == frame_count;
        if (!eos)
          fillNv12Frame(buffer, capacity, width, height, queued_frames);
        status = AMediaCodec_queueInputBuffer(
            resources.codec, static_cast<size_t>(input), 0,
            eos ? 0 : frame_size,
            static_cast<uint64_t>(queued_frames) * 1000000ULL / 15ULL,
            eos ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0);
        if (status != AMEDIA_OK) {
          error_ = mediaError("queue H.264 encoder input", status);
          return false;
        }
        if (eos)
          eos_queued = true;
        else
          ++queued_frames;
      }
    }

    AMediaCodecBufferInfo info{};
    const ssize_t output =
        AMediaCodec_dequeueOutputBuffer(resources.codec, &info, 10000);
    if (output >= 0) {
      size_t capacity = 0;
      const uint8_t *buffer = AMediaCodec_getOutputBuffer(
          resources.codec, static_cast<size_t>(output), &capacity);
      if (info.size > 0 &&
          (info.offset < 0 ||
           static_cast<size_t>(info.offset) + info.size > capacity ||
           !buffer)) {
        error_ = "H.264 encoder returned an invalid output buffer";
        return false;
      }
      if (info.size > 0) {
        result.encoded_bytes += static_cast<size_t>(info.size);
        ++result.output_buffers;
        EncodedPacket packet;
        packet.bytes.assign(buffer + info.offset,
                            buffer + info.offset + info.size);
        packet.presentation_time_us = info.presentationTimeUs;
        packet.flags = info.flags;
        packets.push_back(std::move(packet));
      }
      eos_received = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
      status = AMediaCodec_releaseOutputBuffer(
          resources.codec, static_cast<size_t>(output), false);
      if (status != AMEDIA_OK) {
        error_ = mediaError("release H.264 encoder output", status);
        return false;
      }
    }
  }
  if (!eos_received) {
    error_ = "H.264 encoder did not reach EOS before timeout";
    return false;
  }
  if (queued_frames != frame_count || result.encoded_bytes == 0) {
    error_ = "H.264 encoder produced no usable output";
    return false;
  }
  result.encoder_hardware_accelerated =
      isHardwareComponent(result.encoder_name);
  result.width = width;
  result.height = height;
  result.input_frames = queued_frames;
  return decodeH264(packets, width, height, frame_count, timeout_ms, result,
                    error_);
}

const std::string &CodecManager::lastError() const { return error_; }

} // namespace oos::hardware
