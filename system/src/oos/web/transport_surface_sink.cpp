#include "oos/web/transport_surface_sink.h"

#include "oos/compositor/surface_transport.h"

#include <android/hardware_buffer.h>
#include <glib-unix.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/socket.h>
#include <unistd.h>

namespace oos::web {

TransportSurfaceSink::TransportSurfaceSink(int socket_fd, size_t max_in_flight)
    : socket_fd_(socket_fd),
      max_in_flight_(std::min(max_in_flight, size_t{4})) {}

TransportSurfaceSink::~TransportSurfaceSink() {
  const bool trace = std::getenv("OOS_TRACE_WPE_FRAMES") &&
                     std::strcmp(std::getenv("OOS_TRACE_WPE_FRAMES"), "0") != 0;
  if (trace)
    std::fprintf(stderr, "WPE transport shutdown: source=%u pending=%zu\n",
                 source_id_, pendingCount());
  if (source_id_)
    g_source_remove(source_id_);
  if (socket_fd_ >= 0) {
    shutdown(socket_fd_, SHUT_RDWR);
    close(socket_fd_);
  }
  buffer_ids_.clear();
  if (trace)
    std::fprintf(stderr, "WPE transport shutdown: complete\n");
}

bool TransportSurfaceSink::initialize() {
  if (socket_fd_ < 0 || max_in_flight_ == 0)
    return false;
  if (source_id_)
    return true;
  source_id_ = g_unix_fd_add(
      socket_fd_, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
      receiveAvailable, this);
  return source_id_ != 0;
}

int64_t TransportSurfaceSink::monotonicTimeNs() {
  timespec time{};
  return clock_gettime(CLOCK_MONOTONIC, &time) == 0
             ? static_cast<int64_t>(time.tv_sec) * 1000000000LL + time.tv_nsec
             : 1;
}

bool TransportSurfaceSink::presentSurface(
    const compositor::SurfaceFrame &frame) {
  struct SynchronousResult {
    bool complete = false;
    bool presented = false;
  } result;
  const auto completion = [](void *data, uint64_t, bool presented) {
    auto *result = static_cast<SynchronousResult *>(data);
    result->presented = presented;
    result->complete = true;
  };
  if (!submitSurface(frame, completion, &result))
    return false;
  while (!result.complete && healthy_) {
    if (!receiveOne(5000))
      break;
  }
  return result.complete && result.presented;
}

bool TransportSurfaceSink::submitSurface(const compositor::SurfaceFrame &frame,
                                         Completion completion, void *data) {
  if (socket_fd_ < 0 || !healthy_ || !frame.buffer || !frame.sequence) {
    if (frame.acquire_fence_fd >= 0)
      close(frame.acquire_fence_fd);
    if (completion)
      completion(data, frame.sequence, false);
    return false;
  }
  if (pendingCount() >= max_in_flight_) {
    if (frame.acquire_fence_fd >= 0)
      close(frame.acquire_fence_fd);
    if (completion)
      completion(data, frame.sequence, false);
    return true;
  }

  const auto [buffer, inserted] =
      buffer_ids_.try_emplace(frame.buffer, next_buffer_id_++);
  const OosSurfaceTransportFrame transport_frame = {
      .surface_id = frame.surface_id,
      .buffer_id = buffer->second,
      .sequence = frame.sequence,
      .submitted_at_ns = monotonicTimeNs(),
      .width = frame.width,
      .height = frame.height,
      .flags = inserted ? OOS_SURFACE_FRAME_NEW_BUFFER : 0u,
      .reserved = 0,
  };
  auto pending = std::find_if(pending_.begin(), pending_.end(),
                              [](const PendingSubmission &submission) {
                                return submission.sequence == 0;
                              });
  if (pending == pending_.end()) {
    if (frame.acquire_fence_fd >= 0)
      close(frame.acquire_fence_fd);
    if (completion)
      completion(data, frame.sequence, false);
    return true;
  }
  *pending = {frame.sequence, completion, data};
  const int result = oos_surface_transport_submit(
      socket_fd_, &transport_frame,
      static_cast<AHardwareBuffer *>(frame.buffer), frame.acquire_fence_fd);
  if (result == 0)
    return true;

  if (inserted)
    buffer_ids_.erase(buffer);
  std::fprintf(stderr, "WPE surface transport submit failed: %d (%s)\n", result,
               std::strerror(-result));
  complete(frame.sequence, false);
  fail("surface submit failed");
  return false;
}

void TransportSurfaceSink::cancelSubmissions(void *data) {
  for (PendingSubmission &submission : pending_) {
    if (submission.sequence && submission.data == data)
      submission = {};
  }
}

gboolean TransportSurfaceSink::receiveAvailable(gint, GIOCondition condition,
                                                gpointer data) {
  auto *sink = static_cast<TransportSurfaceSink *>(data);
  if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
    sink->fail("surface release channel closed");
    sink->source_id_ = 0;
    return G_SOURCE_REMOVE;
  }
  while (sink->healthy_) {
    OosSurfaceTransportRelease release{};
    const int result =
        oos_surface_transport_receive_release(sink->socket_fd_, &release, 0);
    if (result == -ETIMEDOUT)
      return G_SOURCE_CONTINUE;
    if (result <= 0) {
      sink->fail(result == 0 ? "surface release channel closed"
                             : "invalid surface release packet");
      sink->source_id_ = 0;
      return G_SOURCE_REMOVE;
    }
    sink->complete(release.sequence, release.accepted != 0);
  }
  sink->source_id_ = 0;
  return G_SOURCE_REMOVE;
}

bool TransportSurfaceSink::receiveOne(int timeout_ms) {
  OosSurfaceTransportRelease release{};
  const int result =
      oos_surface_transport_receive_release(socket_fd_, &release, timeout_ms);
  if (result == 1) {
    complete(release.sequence, release.accepted != 0);
    return true;
  }
  fail(result == 0 ? "surface release channel closed"
                   : "surface release receive failed");
  return false;
}

void TransportSurfaceSink::complete(uint64_t sequence, bool presented) {
  auto submission = std::find_if(
      pending_.begin(), pending_.end(),
      [sequence](const auto &pending) { return pending.sequence == sequence; });
  if (submission == pending_.end())
    return;
  const PendingSubmission callback = *submission;
  *submission = {};
  if (callback.completion)
    callback.completion(callback.data, sequence, presented);
}

void TransportSurfaceSink::fail(const char *message) {
  if (!healthy_)
    return;
  healthy_ = false;
  error_ = message;
  for (PendingSubmission &submission : pending_) {
    if (submission.sequence)
      complete(submission.sequence, false);
  }
  if (failure_handler_)
    failure_handler_(failure_data_);
}

size_t TransportSurfaceSink::pendingCount() const {
  return static_cast<size_t>(
      std::count_if(pending_.begin(), pending_.end(),
                    [](const PendingSubmission &submission) {
                      return submission.sequence != 0;
                    }));
}

} // namespace oos::web
