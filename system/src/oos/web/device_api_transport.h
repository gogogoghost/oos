#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  OOS_DEVICE_API_LIST_FILES = 1,
  OOS_DEVICE_API_READ_FILE = 2,
};

enum {
  OOS_DEVICE_API_INTERNAL = 0,
  OOS_DEVICE_API_REMOVABLE = 1,
};

typedef struct OosDeviceApiRequest {
  uint16_t operation;
  uint16_t volume;
  char path[4097];
} OosDeviceApiRequest;

int oos_device_api_socket_pair(int sockets[2]);

// Performs one synchronous request. On success, *payload is allocated with
// malloc and must be released with oos_device_api_free().
int oos_device_api_request(int socket_fd, uint16_t operation, uint16_t volume,
                           const char *path, void **payload,
                           uint32_t *payload_size, int timeout_ms);

// Returns 1 for a request, 0 after peer shutdown, or a negative errno value.
int oos_device_api_receive(int socket_fd, OosDeviceApiRequest *request,
                           int timeout_ms);
int oos_device_api_reply(int socket_fd, int32_t status, const void *payload,
                         uint32_t payload_size, int timeout_ms);
void oos_device_api_free(void *payload);

#ifdef __cplusplus
}
#endif
