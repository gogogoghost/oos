#include "oos/media/audio_decoder.h"

#include <miniaudio.h>

#include <limits>

namespace oos::media {

class AudioDecoder::Impl {
public:
  void close() {
    if (initialized)
      ma_decoder_uninit(&decoder);
    decoder = {};
    decoded_format = {};
    initialized = false;
  }

  ma_decoder decoder{};
  DecodedAudioFormat decoded_format;
  std::string error;
  bool initialized = false;
};

AudioDecoder::AudioDecoder() : impl_(std::make_unique<Impl>()) {}

AudioDecoder::~AudioDecoder() { impl_->close(); }

bool AudioDecoder::open(const uint8_t *encoded, size_t encoded_bytes) {
  impl_->close();
  impl_->error.clear();
  if (!encoded || encoded_bytes == 0) {
    impl_->error = "encoded audio is empty";
    return false;
  }
  ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 0, 0);
  const ma_result result =
      ma_decoder_init_memory(encoded, encoded_bytes, &config, &impl_->decoder);
  if (result != MA_SUCCESS) {
    impl_->error = std::string("unsupported or invalid encoded audio: ") +
                   ma_result_description(result);
    return false;
  }
  impl_->initialized = true;
  ma_format output_format = ma_format_unknown;
  ma_uint32 channels = 0;
  ma_uint32 sample_rate = 0;
  if (ma_decoder_get_data_format(&impl_->decoder, &output_format, &channels,
                                 &sample_rate, nullptr, 0) != MA_SUCCESS ||
      output_format != ma_format_s16 || channels == 0 || channels > 8 ||
      sample_rate == 0) {
    impl_->error = "decoded audio has an unsupported PCM format";
    impl_->close();
    return false;
  }
  impl_->decoded_format = {sample_rate, channels};
  return true;
}

bool AudioDecoder::openFile(const std::string &path) {
  impl_->close();
  impl_->error.clear();
  if (path.empty()) {
    impl_->error = "audio path is empty";
    return false;
  }
  ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 0, 0);
  const ma_result result =
      ma_decoder_init_file(path.c_str(), &config, &impl_->decoder);
  if (result != MA_SUCCESS) {
    impl_->error =
        std::string("open encoded audio: ") + ma_result_description(result);
    return false;
  }
  impl_->initialized = true;
  ma_format output_format = ma_format_unknown;
  ma_uint32 channels = 0;
  ma_uint32 sample_rate = 0;
  if (ma_decoder_get_data_format(&impl_->decoder, &output_format, &channels,
                                 &sample_rate, nullptr, 0) != MA_SUCCESS ||
      output_format != ma_format_s16 || channels == 0 || channels > 8 ||
      sample_rate == 0) {
    impl_->error = "decoded audio has an unsupported PCM format";
    impl_->close();
    return false;
  }
  impl_->decoded_format = {sample_rate, channels};
  return true;
}

void AudioDecoder::close() { impl_->close(); }

bool AudioDecoder::read(int16_t *samples, uint64_t frame_capacity,
                        uint64_t &frames_read) {
  frames_read = 0;
  if (!impl_->initialized || (!samples && frame_capacity != 0)) {
    impl_->error = "audio decoder is not open or output is invalid";
    return false;
  }
  if (frame_capacity > std::numeric_limits<ma_uint64>::max()) {
    impl_->error = "audio decoder read is too large";
    return false;
  }
  ma_uint64 native_frames = 0;
  const ma_result result = ma_decoder_read_pcm_frames(
      &impl_->decoder, samples, static_cast<ma_uint64>(frame_capacity),
      &native_frames);
  if (result != MA_SUCCESS && result != MA_AT_END) {
    impl_->error =
        std::string("decode audio: ") + ma_result_description(result);
    return false;
  }
  frames_read = native_frames;
  return true;
}

bool AudioDecoder::seek(uint64_t frame) {
  if (!impl_->initialized) {
    impl_->error = "audio decoder is not open";
    return false;
  }
  const ma_result result = ma_decoder_seek_to_pcm_frame(
      &impl_->decoder, static_cast<ma_uint64>(frame));
  if (result == MA_SUCCESS)
    return true;
  impl_->error = std::string("seek audio: ") + ma_result_description(result);
  return false;
}

uint64_t AudioDecoder::lengthFrames() const {
  if (!impl_->initialized)
    return 0;
  ma_uint64 frames = 0;
  return ma_decoder_get_length_in_pcm_frames(&impl_->decoder, &frames) ==
                 MA_SUCCESS
             ? frames
             : 0;
}

const DecodedAudioFormat &AudioDecoder::format() const {
  return impl_->decoded_format;
}

const std::string &AudioDecoder::lastError() const { return impl_->error; }

bool AudioDecoder::opened() const { return impl_->initialized; }

} // namespace oos::media
