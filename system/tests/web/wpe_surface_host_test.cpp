#include <android/hardware_buffer.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <unordered_map>

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
  std::unordered_map<uint64_t, AHardwareBuffer *> surface_buffers;
  bool success = true;
  while (true) {
    OosSurfaceTransportFrame packet{};
    AHardwareBuffer *buffer = nullptr;
    int acquire_fence_fd = -1;
    const int receive_result = oos_surface_transport_receive(
        connection, &packet, &buffer, &acquire_fence_fd, 15000);
    if (receive_result == 0)
      break;
    if (receive_result < 0) {
      std::fprintf(stderr, "failed to receive WPE surface: %d (%s)\n",
                   receive_result, std::strerror(-receive_result));
      success = false;
      break;
    }

    if (buffer) {
      auto [entry, inserted] =
          surface_buffers.emplace(packet.buffer_id, buffer);
      if (!inserted) {
        AHardwareBuffer_release(entry->second);
        entry->second = buffer;
      }
    }
    auto buffer_entry = surface_buffers.find(packet.buffer_id);
    if (buffer_entry == surface_buffers.end()) {
      std::fprintf(stderr, "frame referenced unknown buffer %llu\n",
                   static_cast<unsigned long long>(packet.buffer_id));
      success = false;
      break;
    }
    buffer = buffer_entry->second;
    AHardwareBuffer_Desc description{};
    AHardwareBuffer_describe(buffer, &description);
    oos::compositor::SurfaceFrame frame;
    frame.surface_id = packet.surface_id;
    frame.sequence = packet.sequence;
    frame.buffer = buffer;
    frame.buffer_width = description.width;
    frame.buffer_height = description.height;
    frame.acquire_fence_fd = acquire_fence_fd;
    frame.width = packet.width;
    frame.height = packet.height;
    const bool presented = compositor.presentSurface(frame);
    const OosSurfaceTransportRelease release = {
        .sequence = packet.sequence,
        .presented_at_ns = packet.submitted_at_ns,
        .accepted = static_cast<uint8_t>(presented),
        .reserved = {},
    };
    if (oos_surface_transport_release(connection, &release) != 0) {
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
  for (const auto &[buffer_id, retained_buffer] : surface_buffers) {
    (void)buffer_id;
    AHardwareBuffer_release(retained_buffer);
  }
  close(listener);
  unlink(socket_path);
  std::fprintf(stderr, "OOS compositor host presented %llu WPE frames\n",
               static_cast<unsigned long long>(presented_frames));
  device->shutdown();
  return success && presented_frames > 0 ? 0 : 1;
}
