#include "oos/compositor/surface_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *buffer,
                                           int socket_fd);
int AHardwareBuffer_recvHandleFromUnixSocket(int socket_fd,
                                             AHardwareBuffer **out_buffer);

enum {
  OOS_SURFACE_TRANSPORT_MAGIC = 0x4f535446,
  OOS_SURFACE_TRANSPORT_VERSION = 1,
};

typedef struct OosSurfaceTransportPacket {
  uint32_t magic;
  uint32_t version;
  OosSurfaceTransportFrame frame;
} OosSurfaceTransportPacket;

static int wait_for_fd(int fd, short events, int timeout_ms) {
  struct pollfd descriptor = {.fd = fd, .events = events};
  int result;
  do {
    result = poll(&descriptor, 1, timeout_ms);
  } while (result < 0 && errno == EINTR);
  if (result == 0)
    return -ETIMEDOUT;
  if (result < 0)
    return -errno;
  if (descriptor.revents & (POLLERR | POLLNVAL))
    return -EIO;
  return 0;
}

static int initialize_address(const char *socket_path,
                              struct sockaddr_un *address) {
  if (!socket_path || !socket_path[0] || !address)
    return -EINVAL;
  const size_t length = strlen(socket_path);
  if (length >= sizeof(address->sun_path))
    return -ENAMETOOLONG;
  memset(address, 0, sizeof(*address));
  address->sun_family = AF_UNIX;
  memcpy(address->sun_path, socket_path, length + 1);
  return 0;
}

static int create_socket(void) {
  int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  if (fd < 0)
    return -errno;
  const int flags = fcntl(fd, F_GETFD);
  if (flags >= 0)
    fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  return fd;
}

int oos_surface_transport_listen(const char *socket_path) {
  struct sockaddr_un address;
  int result = initialize_address(socket_path, &address);
  if (result)
    return result;
  int fd = create_socket();
  if (fd < 0)
    return fd;
  unlink(socket_path);
  if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(fd, 1) != 0) {
    result = -errno;
    close(fd);
    unlink(socket_path);
    return result;
  }
  return fd;
}

int oos_surface_transport_accept(int listener_fd, int timeout_ms) {
  int result = wait_for_fd(listener_fd, POLLIN, timeout_ms);
  if (result)
    return result;
  int fd;
  do {
    fd = accept(listener_fd, NULL, NULL);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0)
    return -errno;
  const int flags = fcntl(fd, F_GETFD);
  if (flags >= 0)
    fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  return fd;
}

int oos_surface_transport_connect(const char *socket_path, int timeout_ms) {
  struct sockaddr_un address;
  int result = initialize_address(socket_path, &address);
  if (result)
    return result;
  int fd = create_socket();
  if (fd < 0)
    return fd;
  const int attempts = timeout_ms > 0 ? timeout_ms / 20 + 1 : 1;
  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0)
      return fd;
    result = -errno;
    if (errno != ENOENT && errno != ECONNREFUSED)
      break;
    usleep(20000);
  }
  close(fd);
  return result;
}

int oos_surface_transport_send(int socket_fd,
                               const OosSurfaceTransportFrame *frame,
                               AHardwareBuffer *buffer, int acquire_fence_fd,
                               int timeout_ms) {
  if (socket_fd < 0 || !frame || !buffer || !frame->surface_id ||
      !frame->width || !frame->height) {
    if (acquire_fence_fd >= 0)
      close(acquire_fence_fd);
    return -EINVAL;
  }
  if (acquire_fence_fd >= 0) {
    const int fence_result = wait_for_fd(acquire_fence_fd, POLLIN, timeout_ms);
    close(acquire_fence_fd);
    if (fence_result)
      return fence_result;
  }
  const OosSurfaceTransportPacket packet = {
      .magic = OOS_SURFACE_TRANSPORT_MAGIC,
      .version = OOS_SURFACE_TRANSPORT_VERSION,
      .frame = *frame,
  };
  ssize_t sent;
  do {
    sent = send(socket_fd, &packet, sizeof(packet), MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  if (sent != (ssize_t)sizeof(packet))
    return sent < 0 ? -errno : -EIO;
  int result = AHardwareBuffer_sendHandleToUnixSocket(buffer, socket_fd);
  if (result)
    return result;
  result = wait_for_fd(socket_fd, POLLIN, timeout_ms);
  if (result)
    return result;
  uint8_t accepted = 0;
  ssize_t received;
  do {
    received = recv(socket_fd, &accepted, sizeof(accepted), 0);
  } while (received < 0 && errno == EINTR);
  if (received != (ssize_t)sizeof(accepted))
    return received < 0 ? -errno : -EPIPE;
  return accepted ? 0 : -ECANCELED;
}

int oos_surface_transport_receive(int socket_fd,
                                  OosSurfaceTransportFrame *frame,
                                  AHardwareBuffer **buffer, int timeout_ms) {
  if (socket_fd < 0 || !frame || !buffer)
    return -EINVAL;
  *buffer = NULL;
  int result = wait_for_fd(socket_fd, POLLIN, timeout_ms);
  if (result)
    return result;
  OosSurfaceTransportPacket packet;
  ssize_t received;
  do {
    received = recv(socket_fd, &packet, sizeof(packet), 0);
  } while (received < 0 && errno == EINTR);
  if (received == 0)
    return 0;
  if (received != (ssize_t)sizeof(packet))
    return received < 0 ? -errno : -EPROTO;
  if (packet.magic != OOS_SURFACE_TRANSPORT_MAGIC ||
      packet.version != OOS_SURFACE_TRANSPORT_VERSION ||
      !packet.frame.surface_id || !packet.frame.width || !packet.frame.height)
    return -EPROTO;
  result = wait_for_fd(socket_fd, POLLIN, timeout_ms);
  if (result)
    return result;
  result = AHardwareBuffer_recvHandleFromUnixSocket(socket_fd, buffer);
  if (result)
    return result;
  *frame = packet.frame;
  return 1;
}

int oos_surface_transport_acknowledge(int socket_fd, int accepted) {
  const uint8_t value = accepted ? 1 : 0;
  ssize_t sent;
  do {
    sent = send(socket_fd, &value, sizeof(value), MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  return sent == (ssize_t)sizeof(value) ? 0 : (sent < 0 ? -errno : -EIO);
}
