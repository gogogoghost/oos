#include "oos/compositor/surface_transport.h"

#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>

int main() {
  int sockets[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) != 0) {
    std::perror("socketpair");
    return 1;
  }
  const OosSurfaceTransportKey expected = {
      .timestamp_us = 123456789,
      .code = 28,
      .action = 1,
      .reserved = 0,
  };
  OosSurfaceTransportKey actual = {};
  const int sent = oos_surface_transport_send_key(sockets[0], &expected);
  const int received =
      oos_surface_transport_receive_key(sockets[1], &actual, 1000);
  close(sockets[0]);
  close(sockets[1]);
  if (sent != 0 || received != 1 ||
      actual.timestamp_us != expected.timestamp_us ||
      actual.code != expected.code || actual.action != expected.action) {
    std::fprintf(stderr, "FAIL: surface transport key round trip\n");
    return 1;
  }
  std::fprintf(stderr, "PASS: surface transport key round trip\n");
  return 0;
}
