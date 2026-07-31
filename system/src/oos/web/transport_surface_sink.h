#pragma once

#include "oos/compositor/surface.h"

#include <glib-unix.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace oos::web {

class TransportSurfaceSink final : public compositor::SurfaceSink {
public:
  using FailureHandler = void (*)(void *data);

  explicit TransportSurfaceSink(int socket_fd, size_t max_in_flight = 3);
  ~TransportSurfaceSink() override;

  TransportSurfaceSink(const TransportSurfaceSink &) = delete;
  TransportSurfaceSink &operator=(const TransportSurfaceSink &) = delete;

  bool initialize();
  bool healthy() const { return healthy_; }
  const char *lastError() const { return error_; }
  void setFailureHandler(FailureHandler handler, void *data) {
    failure_handler_ = handler;
    failure_data_ = data;
  }

  bool presentSurface(const compositor::SurfaceFrame &frame) override;
  bool submitSurface(const compositor::SurfaceFrame &frame,
                     Completion completion, void *data) override;
  void cancelSubmissions(void *data) override;

private:
  struct PendingSubmission {
    uint64_t sequence = 0;
    Completion completion = nullptr;
    void *data = nullptr;
  };

  static int64_t monotonicTimeNs();
  static gboolean receiveAvailable(gint fd, GIOCondition condition,
                                   gpointer data);
  bool receiveOne(int timeout_ms);
  void complete(uint64_t sequence, bool presented);
  void fail(const char *message);
  size_t pendingCount() const;

  int socket_fd_ = -1;
  unsigned int source_id_ = 0;
  size_t max_in_flight_ = 3;
  uint64_t next_buffer_id_ = 1;
  std::unordered_map<void *, uint64_t> buffer_ids_;
  std::array<PendingSubmission, 4> pending_{};
  bool healthy_ = true;
  const char *error_ = "";
  FailureHandler failure_handler_ = nullptr;
  void *failure_data_ = nullptr;
};

} // namespace oos::web
