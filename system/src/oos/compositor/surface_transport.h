#pragma once

#include <android/hardware_buffer.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OosSurfaceTransportFrame {
  uint64_t surface_id;
  uint64_t buffer_id;
  uint64_t sequence;
  int64_t submitted_at_ns;
  uint32_t width;
  uint32_t height;
  uint32_t flags;
  uint32_t reserved;
} OosSurfaceTransportFrame;

typedef struct OosSurfaceTransportRelease {
  uint64_t sequence;
  int64_t presented_at_ns;
  uint8_t accepted;
  uint8_t reserved[7];
} OosSurfaceTransportRelease;

enum {
  // The frame is followed by an AHardwareBuffer handle. Producers set this
  // only for the first frame using a buffer_id; hosts retain that buffer until
  // the surface connection closes.
  OOS_SURFACE_FRAME_NEW_BUFFER = 1u << 0,
};

typedef struct OosSurfaceTransportKey {
  int64_t timestamp_us;
  uint16_t code;
  uint8_t action;
  uint8_t reserved;
} OosSurfaceTransportKey;

// Creates one connected SOCK_SEQPACKET channel. Surface traffic and input
// traffic must use different pairs so release and key packets have one reader.
int oos_surface_transport_socket_pair(int sockets[2]);

int oos_surface_transport_listen(const char *socket_path);
int oos_surface_transport_accept(int listener_fd, int timeout_ms);
int oos_surface_transport_connect(const char *socket_path, int timeout_ms);

// Submits without waiting for presentation. The acquire fence is always
// consumed and transferred to the host when present. The producer must not
// reuse the frame buffer until its matching release arrives.
int oos_surface_transport_submit(int socket_fd,
                                 const OosSurfaceTransportFrame *frame,
                                 AHardwareBuffer *buffer, int acquire_fence_fd);

// Returns 1 for a frame, 0 when the producer closed the connection, or a
// negative errno value. A non-null returned buffer is a new retained pool
// entry owned by the caller; null means the caller must resolve buffer_id from
// its connection-local pool.
int oos_surface_transport_receive(int socket_fd,
                                  OosSurfaceTransportFrame *frame,
                                  AHardwareBuffer **buffer,
                                  int *acquire_fence_fd, int timeout_ms);

int oos_surface_transport_release(int socket_fd,
                                  const OosSurfaceTransportRelease *release);
int oos_surface_transport_receive_release(int socket_fd,
                                          OosSurfaceTransportRelease *release,
                                          int timeout_ms);

// Input uses a dedicated channel. The OOS host remains the only process that
// reads evdev; the WPE producer receives normalized raw key codes and maps them
// to WPE key symbols.
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
