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

typedef struct OosSurfaceTransportKey {
  int64_t timestamp_us;
  uint16_t code;
  uint8_t action;
  uint8_t reserved;
} OosSurfaceTransportKey;

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

// Input travels in the opposite direction from frames. The OOS host remains
// the only process that reads evdev; the WPE producer receives normalized raw
// key codes and maps them to WPE key symbols.
int oos_surface_transport_send_key(int socket_fd,
                                   const OosSurfaceTransportKey *key);
// Returns 1 for a key, 0 when the host closed the connection, or a negative
// errno value. A zero timeout performs a non-blocking poll.
int oos_surface_transport_receive_key(int socket_fd,
                                      OosSurfaceTransportKey *key,
                                      int timeout_ms);

#ifdef __cplusplus
}
#endif
