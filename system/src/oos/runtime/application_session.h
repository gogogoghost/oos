#pragma once

#include "oos/window/input_router.h"

#include <cstdint>
#include <string>

namespace oos::runtime {

class ApplicationSession : public window::ApplicationInputTarget {
public:
  ~ApplicationSession() override = default;

  virtual bool initialize() = 0;
  virtual void shutdown() = 0;
  virtual bool frame(int64_t monotonic_us, uint32_t &next_delay_ms) = 0;
  virtual std::string takeLaunchRequest() = 0;
};

} // namespace oos::runtime
