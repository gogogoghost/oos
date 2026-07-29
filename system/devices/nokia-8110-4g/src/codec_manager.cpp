#include "oos/hardware/codec_manager.h"

namespace oos::hardware {

bool CodecManager::testH264RoundTrip(int, int, int, CodecResult &result, int) {
  result = {};
  error_ = "hardware video codec is not implemented on nokia-8110-4g";
  return false;
}

const std::string &CodecManager::lastError() const { return error_; }

} // namespace oos::hardware
