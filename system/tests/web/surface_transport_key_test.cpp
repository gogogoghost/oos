#include "oos/compositor/surface_transport.h"

#include <android/hardware_buffer.h>

#include <cerrno>
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *description,
                             AHardwareBuffer **buffer);
void AHardwareBuffer_release(AHardwareBuffer *buffer);
}

namespace {

bool testRetainedBufferTransport() {
  int sockets[2] = {-1, -1};
  if (oos_surface_transport_socket_pair(sockets) != 0)
    return false;

  AHardwareBuffer_Desc description = {};
  description.width = 16;
  description.height = 16;
  description.layers = 1;
  description.format = AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM;
  description.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                      AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
  AHardwareBuffer *producer_buffer = nullptr;
  if (AHardwareBuffer_allocate(&description, &producer_buffer) != 0 ||
      !producer_buffer) {
    close(sockets[0]);
    close(sockets[1]);
    return false;
  }

  const pid_t consumer = fork();
  if (consumer == 0) {
    close(sockets[0]);
    OosSurfaceTransportFrame received_frame = {};
    AHardwareBuffer *retained_buffer = nullptr;
    int acquire_fence = -1;
    const int allocation = oos_surface_transport_receive(
        sockets[1], &received_frame, &retained_buffer, &acquire_fence, 2000);
    const bool first_valid =
        allocation == 1 && retained_buffer && received_frame.buffer_id == 7 &&
        received_frame.sequence == 11 && acquire_fence >= 0 &&
        (received_frame.flags & OOS_SURFACE_FRAME_NEW_BUFFER);
    if (acquire_fence >= 0)
      close(acquire_fence);
    const OosSurfaceTransportRelease first_release = {
        .sequence = received_frame.sequence,
        .presented_at_ns = 200,
        .accepted = static_cast<uint8_t>(first_valid),
        .reserved = {},
    };
    if (oos_surface_transport_release(sockets[1], &first_release) != 0) {
      if (retained_buffer)
        AHardwareBuffer_release(retained_buffer);
      _exit(1);
    }

    AHardwareBuffer *unexpected_buffer = nullptr;
    acquire_fence = -1;
    const int reference = oos_surface_transport_receive(
        sockets[1], &received_frame, &unexpected_buffer, &acquire_fence, 2000);
    const bool second_valid =
        reference == 1 && !unexpected_buffer && acquire_fence < 0 &&
        received_frame.buffer_id == 7 && received_frame.sequence == 12 &&
        received_frame.flags == 0;
    const OosSurfaceTransportRelease second_release = {
        .sequence = received_frame.sequence,
        .presented_at_ns = 300,
        .accepted = static_cast<uint8_t>(second_valid),
        .reserved = {},
    };
    const bool consumer_success =
        oos_surface_transport_release(sockets[1], &second_release) == 0 &&
        first_valid && second_valid;
    if (retained_buffer)
      AHardwareBuffer_release(retained_buffer);
    close(sockets[1]);
    _exit(consumer_success ? 0 : 1);
  }
  if (consumer < 0) {
    AHardwareBuffer_release(producer_buffer);
    close(sockets[0]);
    close(sockets[1]);
    return false;
  }
  close(sockets[1]);

  int fence_pipe[2] = {-1, -1};
  if (pipe(fence_pipe) != 0) {
    AHardwareBuffer_release(producer_buffer);
    close(sockets[0]);
    return false;
  }
  const char signal = 1;
  (void)write(fence_pipe[1], &signal, sizeof(signal));
  close(fence_pipe[1]);

  OosSurfaceTransportFrame frame = {
      .surface_id = 1,
      .buffer_id = 7,
      .sequence = 11,
      .submitted_at_ns = 100,
      .width = 16,
      .height = 16,
      .flags = OOS_SURFACE_FRAME_NEW_BUFFER,
      .reserved = 0,
  };
  const int allocation = oos_surface_transport_submit(
      sockets[0], &frame, producer_buffer, fence_pipe[0]);
  OosSurfaceTransportRelease release = {};
  const int first_released =
      allocation == 0
          ? oos_surface_transport_receive_release(sockets[0], &release, 2000)
          : allocation;
  frame.flags = 0;
  frame.sequence = 12;
  const int reference =
      first_released == 1 && release.sequence == 11 && release.accepted
          ? oos_surface_transport_submit(sockets[0], &frame, nullptr, -1)
          : -1;
  const int second_released =
      reference == 0
          ? oos_surface_transport_receive_release(sockets[0], &release, 2000)
          : reference;
  int consumer_status = 0;
  while (waitpid(consumer, &consumer_status, 0) < 0 && errno == EINTR) {
  }
  AHardwareBuffer_release(producer_buffer);
  close(sockets[0]);
  return allocation == 0 && first_released == 1 && reference == 0 &&
         second_released == 1 && release.sequence == 12 && release.accepted &&
         WIFEXITED(consumer_status) && WEXITSTATUS(consumer_status) == 0;
}

} // namespace

int main() {
  int surface_sockets[2] = {-1, -1};
  int input_sockets[2] = {-1, -1};
  if (oos_surface_transport_socket_pair(surface_sockets) != 0 ||
      oos_surface_transport_socket_pair(input_sockets) != 0) {
    std::fprintf(stderr, "FAIL: create isolated transport channels\n");
    return 1;
  }

  bool success = true;
  for (uint16_t index = 0; index < 1024; ++index) {
    const OosSurfaceTransportKey expected = {
        .timestamp_us = 123456789 + index,
        .code = index,
        .action = static_cast<uint8_t>(index % 3),
        .reserved = 0,
    };
    OosSurfaceTransportKey actual = {};
    const int sent =
        oos_surface_transport_send_key(input_sockets[0], &expected);
    const int surface_received =
        oos_surface_transport_receive_key(surface_sockets[1], &actual, 0);
    const int received =
        oos_surface_transport_receive_key(input_sockets[1], &actual, 1000);
    if (sent != 0 || surface_received != -ETIMEDOUT || received != 1 ||
        actual.timestamp_us != expected.timestamp_us ||
        actual.code != expected.code || actual.action != expected.action) {
      success = false;
      break;
    }
  }
  close(surface_sockets[0]);
  close(surface_sockets[1]);
  close(input_sockets[0]);
  close(input_sockets[1]);
  std::fprintf(stderr, "%s: isolated input transport stress test\n",
               success ? "PASS" : "FAIL");
  const bool retained_buffer_success = testRetainedBufferTransport();
  std::fprintf(stderr, "%s: retained surface buffer transport test\n",
               retained_buffer_success ? "PASS" : "FAIL");
  return success && retained_buffer_success ? 0 : 1;
}
