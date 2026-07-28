#pragma once

#include <cstddef>
#include <string>

namespace oos::hardware {

struct CodecResult {
  std::string encoder_name;
  std::string decoder_name;
  bool encoder_hardware_accelerated = false;
  bool decoder_hardware_accelerated = false;
  int width = 0;
  int height = 0;
  int input_frames = 0;
  int output_buffers = 0;
  int decoded_frames = 0;
  size_t encoded_bytes = 0;
};

class CodecManager {
public:
  // Encodes generated NV12 frames, then decodes them and drains both to EOS.
  bool testH264RoundTrip(int width, int height, int frame_count,
                         CodecResult &result, int timeout_ms = 15000);

  const std::string &lastError() const;

private:
  std::string error_;
};

} // namespace oos::hardware
