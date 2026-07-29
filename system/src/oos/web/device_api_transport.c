#include "oos/web/device_api_transport.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define OOS_DEVICE_API_MAGIC 0x4f4f5341u
#define OOS_DEVICE_API_VERSION 2u
#define OOS_DEVICE_API_MAX_PATH 4096u
#define OOS_DEVICE_API_MAX_PAYLOAD (64u * 1024u * 1024u)

typedef struct RequestHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t operation;
  uint16_t volume;
  uint16_t flags;
  uint32_t path_size;
  uint32_t payload_size;
} RequestHeader;

typedef struct ResponseHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  int32_t status;
  uint32_t payload_size;
} ResponseHeader;

static int wait_for_fd(int fd, short events, int timeout_ms) {
  struct pollfd descriptor = {.fd = fd, .events = events, .revents = 0};
  int result;
  do {
    result = poll(&descriptor, 1, timeout_ms);
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    return -errno;
  if (result == 0)
    return -ETIMEDOUT;
  if (descriptor.revents & (POLLERR | POLLNVAL))
    return -EIO;
  if ((descriptor.revents & POLLHUP) && !(descriptor.revents & events))
    return 0;
  return 1;
}

static int send_exact(int fd, const void *data, size_t size, int timeout_ms) {
  const uint8_t *cursor = data;
  while (size) {
    int result = wait_for_fd(fd, POLLOUT, timeout_ms);
    if (result <= 0)
      return result == 0 ? -EPIPE : result;
    ssize_t count;
    do {
      count = send(fd, cursor, size, MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR);
    if (count <= 0)
      return count == 0 ? -EPIPE : -errno;
    cursor += count;
    size -= (size_t)count;
  }
  return 0;
}

static int receive_exact(int fd, void *data, size_t size, int timeout_ms) {
  uint8_t *cursor = data;
  while (size) {
    int result = wait_for_fd(fd, POLLIN, timeout_ms);
    if (result <= 0)
      return result;
    ssize_t count;
    do {
      count = recv(fd, cursor, size, 0);
    } while (count < 0 && errno == EINTR);
    if (count <= 0)
      return count == 0 ? 0 : -errno;
    cursor += count;
    size -= (size_t)count;
  }
  return 1;
}

int oos_device_api_socket_pair(int sockets[2]) {
  if (!sockets)
    return -EINVAL;
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
    return -errno;
  return 0;
}

static int valid_operation(uint16_t operation) {
  return operation >= OOS_DEVICE_API_LIST_FILES &&
         operation <= OOS_DEVICE_API_USED_SPACE;
}

int oos_device_api_request_with_payload(
    int socket_fd, uint16_t operation, uint16_t volume, uint16_t flags,
    const char *path, const void *request_payload,
    uint32_t request_payload_size, void **response_payload,
    uint32_t *response_payload_size, int timeout_ms) {
  if (socket_fd < 0 || !response_payload || !response_payload_size ||
      !valid_operation(operation) || volume > OOS_DEVICE_API_REMOVABLE ||
      request_payload_size > OOS_DEVICE_API_MAX_PAYLOAD ||
      (request_payload_size && !request_payload) ||
      (operation != OOS_DEVICE_API_WRITE_FILE && request_payload_size) ||
      (operation == OOS_DEVICE_API_WRITE_FILE &&
       flags > OOS_DEVICE_API_WRITE_APPEND) ||
      (operation != OOS_DEVICE_API_WRITE_FILE && flags))
    return -EINVAL;
  *response_payload = NULL;
  *response_payload_size = 0;
  if (!path)
    path = "";
  const size_t path_size = strlen(path);
  if (path_size > OOS_DEVICE_API_MAX_PATH ||
      ((operation == OOS_DEVICE_API_READ_FILE ||
        operation == OOS_DEVICE_API_WRITE_FILE ||
        operation == OOS_DEVICE_API_DELETE_FILE) &&
       path_size == 0))
    return -EINVAL;
  const RequestHeader request = {
      .magic = OOS_DEVICE_API_MAGIC,
      .version = OOS_DEVICE_API_VERSION,
      .operation = operation,
      .volume = volume,
      .flags = flags,
      .path_size = (uint32_t)path_size,
      .payload_size = request_payload_size,
  };
  int result = send_exact(socket_fd, &request, sizeof(request), timeout_ms);
  if (!result && path_size)
    result = send_exact(socket_fd, path, path_size, timeout_ms);
  if (!result && request_payload_size)
    result = send_exact(socket_fd, request_payload, request_payload_size,
                        timeout_ms);
  if (result)
    return result;

  ResponseHeader response;
  result = receive_exact(socket_fd, &response, sizeof(response), timeout_ms);
  if (result <= 0)
    return result == 0 ? -EPIPE : result;
  if (response.magic != OOS_DEVICE_API_MAGIC ||
      response.version != OOS_DEVICE_API_VERSION ||
      response.payload_size > OOS_DEVICE_API_MAX_PAYLOAD)
    return -EPROTO;
  void *bytes = NULL;
  if (response.payload_size) {
    bytes = malloc(response.payload_size);
    if (!bytes)
      return -ENOMEM;
    result = receive_exact(socket_fd, bytes, response.payload_size, timeout_ms);
    if (result <= 0) {
      free(bytes);
      return result == 0 ? -EPIPE : result;
    }
  }
  if (response.status != 0) {
    free(bytes);
    return response.status < 0 ? response.status : -EIO;
  }
  *response_payload = bytes;
  *response_payload_size = response.payload_size;
  return 0;
}

int oos_device_api_request(int socket_fd, uint16_t operation, uint16_t volume,
                           const char *path, void **payload,
                           uint32_t *payload_size, int timeout_ms) {
  return oos_device_api_request_with_payload(socket_fd, operation, volume, 0,
                                             path, NULL, 0, payload,
                                             payload_size, timeout_ms);
}

int oos_device_api_receive(int socket_fd, OosDeviceApiRequest *request,
                           int timeout_ms) {
  if (socket_fd < 0 || !request)
    return -EINVAL;
  RequestHeader header;
  int result = receive_exact(socket_fd, &header, sizeof(header), timeout_ms);
  if (result <= 0)
    return result;
  if (header.magic != OOS_DEVICE_API_MAGIC ||
      header.version != OOS_DEVICE_API_VERSION ||
      !valid_operation(header.operation) ||
      header.volume > OOS_DEVICE_API_REMOVABLE ||
      header.path_size > OOS_DEVICE_API_MAX_PATH ||
      header.payload_size > OOS_DEVICE_API_MAX_PAYLOAD ||
      (header.operation != OOS_DEVICE_API_WRITE_FILE && header.payload_size) ||
      (header.operation == OOS_DEVICE_API_WRITE_FILE &&
       header.flags > OOS_DEVICE_API_WRITE_APPEND) ||
      (header.operation != OOS_DEVICE_API_WRITE_FILE && header.flags) ||
      ((header.operation == OOS_DEVICE_API_READ_FILE ||
        header.operation == OOS_DEVICE_API_WRITE_FILE ||
        header.operation == OOS_DEVICE_API_DELETE_FILE) &&
       !header.path_size))
    return -EPROTO;
  memset(request, 0, sizeof(*request));
  request->operation = header.operation;
  request->volume = header.volume;
  request->flags = header.flags;
  if (header.path_size) {
    result =
        receive_exact(socket_fd, request->path, header.path_size, timeout_ms);
    if (result <= 0)
      return result == 0 ? -EPIPE : result;
  }
  request->path[header.path_size] = '\0';
  if (header.payload_size) {
    request->payload = malloc(header.payload_size);
    if (!request->payload)
      return -ENOMEM;
    result = receive_exact(socket_fd, request->payload, header.payload_size,
                           timeout_ms);
    if (result <= 0) {
      oos_device_api_request_clear(request);
      return result == 0 ? -EPIPE : result;
    }
    request->payload_size = header.payload_size;
  }
  return 1;
}

void oos_device_api_request_clear(OosDeviceApiRequest *request) {
  if (!request)
    return;
  free(request->payload);
  request->payload = NULL;
  request->payload_size = 0;
}

int oos_device_api_reply(int socket_fd, int32_t status, const void *payload,
                         uint32_t payload_size, int timeout_ms) {
  if (socket_fd < 0 || payload_size > OOS_DEVICE_API_MAX_PAYLOAD ||
      (payload_size && !payload))
    return -EINVAL;
  const ResponseHeader response = {
      .magic = OOS_DEVICE_API_MAGIC,
      .version = OOS_DEVICE_API_VERSION,
      .reserved = 0,
      .status = status,
      .payload_size = payload_size,
  };
  int result = send_exact(socket_fd, &response, sizeof(response), timeout_ms);
  if (!result && payload_size)
    result = send_exact(socket_fd, payload, payload_size, timeout_ms);
  return result;
}

void oos_device_api_free(void *payload) { free(payload); }
