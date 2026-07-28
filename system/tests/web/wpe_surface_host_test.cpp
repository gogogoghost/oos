#include <android/hardware_buffer.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "oos/compositor/compositor.h"
#include "oos/compositor/surface_transport.h"
#include "oos/device/device.h"
#include "oos/device/display.h"

extern "C" {
void AHardwareBuffer_describe(const AHardwareBuffer *buffer,
                              AHardwareBuffer_Desc *description);
void AHardwareBuffer_release(AHardwareBuffer *buffer);
}

namespace {

const char *environmentOr(const char *name, const char *fallback) {
  const char *value = std::getenv(name);
  return value && value[0] ? value : fallback;
}

} // namespace

int main() {
  auto device = oos::device::createDevice();
  oos::device::DeviceInitOptions options;
  options.key_input = false;
  options.grab_input = false;
  if (!device || !device->initialize(options)) {
    std::fprintf(stderr, "failed to initialize OOS device: %s\n",
                 device ? device->lastError().c_str() : "factory unavailable");
    return 1;
  }

  const char *socket_path = environmentOr(
      "OOS_SURFACE_SOCKET", "/data/local/tmp/oos-wpe/wpe-surface.sock");
  const int listener = oos_surface_transport_listen(socket_path);
  if (listener < 0) {
    std::fprintf(stderr, "failed to listen for WPE surfaces: %d (%s)\n",
                 listener, std::strerror(-listener));
    device->shutdown();
    return 1;
  }
  std::fprintf(stderr, "OOS compositor host ready: %s\n", socket_path);
  std::fflush(stderr);
  const int connection = oos_surface_transport_accept(listener, 10000);
  if (connection < 0) {
    std::fprintf(stderr, "failed to accept WPE producer: %d (%s)\n", connection,
                 std::strerror(-connection));
    close(listener);
    unlink(socket_path);
    device->shutdown();
    return 1;
  }

  oos::device::Display &display = device->display();
  oos::compositor::Compositor compositor(display);
  uint64_t presented_frames = 0;
  bool success = true;
  while (true) {
    OosSurfaceTransportFrame packet{};
    AHardwareBuffer *buffer = nullptr;
    const int receive_result =
        oos_surface_transport_receive(connection, &packet, &buffer, 15000);
    if (receive_result == 0)
      break;
    if (receive_result < 0) {
      std::fprintf(stderr, "failed to receive WPE surface: %d (%s)\n",
                   receive_result, std::strerror(-receive_result));
      success = false;
      break;
    }

    AHardwareBuffer_Desc description{};
    AHardwareBuffer_describe(buffer, &description);
    oos::compositor::SurfaceFrame frame;
    frame.surface_id = packet.surface_id;
    frame.buffer = buffer;
    frame.buffer_width = description.width;
    frame.buffer_height = description.height;
    frame.width = packet.width;
    frame.height = packet.height;
    const bool presented = compositor.presentSurface(frame);
    AHardwareBuffer_release(buffer);
    if (oos_surface_transport_acknowledge(connection, presented) != 0) {
      success = false;
      break;
    }
    if (!presented) {
      std::fprintf(stderr, "OOS compositor rejected WPE IPC surface\n");
      success = false;
      break;
    }
    ++presented_frames;
  }

  close(connection);
  close(listener);
  unlink(socket_path);
  std::fprintf(stderr, "OOS compositor host presented %llu WPE frames\n",
               static_cast<unsigned long long>(presented_frames));
  device->shutdown();
  return success && presented_frames > 0 ? 0 : 1;
}
