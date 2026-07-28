#pragma once

#include <android/hardware_buffer.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OosSurfaceTransportFrame {
  uint64_t surface_id;
  uint32_t width;
  uint32_t height;
} OosSurfaceTransportFrame;

int oos_surface_transport_listen(const char *socket_path);
int oos_surface_transport_accept(int listener_fd, int timeout_ms);
int oos_surface_transport_connect(const char *socket_path, int timeout_ms);

// The acquire fence is always consumed. The call returns after the host has
// presented or rejected the buffer, so the producer may release it safely.
int oos_surface_transport_send(int socket_fd,
                               const OosSurfaceTransportFrame *frame,
                               AHardwareBuffer *buffer, int acquire_fence_fd,
                               int timeout_ms);

// Returns 1 for a frame, 0 when the producer closed the connection, or a
// negative errno value. The caller owns the returned buffer reference.
int oos_surface_transport_receive(int socket_fd,
                                  OosSurfaceTransportFrame *frame,
                                  AHardwareBuffer **buffer, int timeout_ms);
int oos_surface_transport_acknowledge(int socket_fd, int accepted);

#ifdef __cplusplus
}
#endif
