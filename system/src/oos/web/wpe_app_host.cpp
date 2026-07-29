#include "oos/web/wpe_app_host.h"

#include "oos/compositor/compositor.h"
#include "oos/compositor/surface_transport.h"
#include "oos/device/device.h"
#include "oos/input/key_input.h"
#include "oos/storage/device_storage.h"
#include "oos/web/device_api_transport.h"

#include <android/hardware_buffer.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" {
void AHardwareBuffer_describe(const AHardwareBuffer *buffer,
                              AHardwareBuffer_Desc *description);
void AHardwareBuffer_release(AHardwareBuffer *buffer);
}

namespace oos::web {
namespace {

constexpr int kAcceptSliceMs = 100;
constexpr int kAcceptTimeoutMs = 10000;
constexpr int kFramePollMs = 16;
constexpr int kChildStopSlices = 50;
constexpr useconds_t kChildStopSliceUs = 20000;
constexpr int kDeviceApiTimeoutMs = 30000;

const char *environmentOr(const char *name, const char *fallback) {
  const char *value = std::getenv(name);
  return value && value[0] ? value : fallback;
}

std::string errorText(const char *operation, int result) {
  return std::string(operation) + ": " + std::strerror(-result);
}

bool childExited(pid_t child, int &status) {
  const pid_t result = waitpid(child, &status, WNOHANG);
  return result == child || (result < 0 && errno == ECHILD);
}

void stopChild(pid_t child) {
  int status = 0;
  if (child <= 0 || childExited(child, status))
    return;
  kill(child, SIGTERM);
  for (int attempt = 0; attempt < kChildStopSlices; ++attempt) {
    if (childExited(child, status))
      return;
    usleep(kChildStopSliceUs);
  }
  kill(child, SIGKILL);
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
}

struct KeyDispatchContext {
  int connection = -1;
  bool success = true;
  bool trace = false;
};

void sendKey(void *data, const input::KeyEvent &event) {
  auto *context = static_cast<KeyDispatchContext *>(data);
  const OosSurfaceTransportKey key = {
      .timestamp_us = event.timestamp_us,
      .code = event.code,
      .action = static_cast<uint8_t>(event.action),
      .reserved = 0,
  };
  const int result = oos_surface_transport_send_key(context->connection, &key);
  if (context->trace) {
    std::fprintf(stderr, "OOS key send: code=%u action=%u result=%d\n",
                 key.code, key.action, result);
  }
  if (result != 0)
    context->success = false;
}

void appendJsonString(std::string &output, const std::string &value) {
  static const char hex[] = "0123456789abcdef";
  output.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20) {
        output += "\\u00";
        output.push_back(hex[character >> 4]);
        output.push_back(hex[character & 0x0f]);
      } else {
        output.push_back(static_cast<char>(character));
      }
    }
  }
  output.push_back('"');
}

std::string
serializeEntries(const std::vector<storage::DeviceStorageEntry> &entries) {
  std::string output = "[";
  for (size_t index = 0; index < entries.size(); ++index) {
    if (index)
      output.push_back(',');
    output += "{\"path\":";
    appendJsonString(output, entries[index].path);
    output += ",\"size\":" + std::to_string(entries[index].size);
    output +=
        ",\"lastModified\":" + std::to_string(entries[index].last_modified_ms) +
        "}";
  }
  output.push_back(']');
  return output;
}

bool serviceDeviceApi(int socket_fd, storage::DeviceStorageService &service,
                      bool &connected, std::string &error) {
  if (!connected)
    return true;
  OosDeviceApiRequest request = {};
  const int received = oos_device_api_receive(socket_fd, &request, 0);
  if (received == -ETIMEDOUT)
    return true;
  if (received == 0) {
    connected = false;
    return true;
  }
  if (received < 0) {
    error = errorText("receive WPE device API request", received);
    return false;
  }

  const auto volume = static_cast<storage::DeviceStorageVolume>(request.volume);
  int status = 0;
  const void *payload = nullptr;
  uint32_t payload_size = 0;
  std::string serialized;
  std::vector<uint8_t> bytes;
  if (request.operation == OOS_DEVICE_API_LIST_FILES) {
    std::vector<storage::DeviceStorageEntry> entries;
    if (!service.list(volume, entries)) {
      status = -ENOENT;
    } else {
      serialized = serializeEntries(entries);
      payload = serialized.data();
      payload_size = static_cast<uint32_t>(serialized.size());
    }
  } else if (!service.read(volume, request.path, bytes)) {
    status = -ENOENT;
  } else {
    payload = bytes.data();
    payload_size = static_cast<uint32_t>(bytes.size());
  }
  const int replied = oos_device_api_reply(socket_fd, status, payload,
                                           payload_size, kDeviceApiTimeoutMs);
  if (replied != 0) {
    error = errorText("reply to WPE device API request", replied);
    return false;
  }
  return true;
}

pid_t startRunner(const apps::AppLaunch &launch,
                  const device::DeviceDescriptor &device,
                  const char *socket_path, int api_fd, int host_api_fd) {
  const char *runner = environmentOr("OOS_WPE_RUNNER", "/opt/oos/bin/oos-wpe");
  const std::string width = std::to_string(device.primary_width);
  const std::string height = std::to_string(device.primary_height);
  const std::string api_descriptor = std::to_string(api_fd);
  std::vector<const char *> arguments = {
      runner,
      "--id",
      launch.app.manifest.id.c_str(),
      "--package",
      launch.executable_path.c_str(),
      "--entrypoint",
      launch.entrypoint.c_str(),
      "--api-profile",
      launch.app.manifest.api_profile.c_str(),
      "--data",
      launch.data_directory.c_str(),
      "--cache",
      launch.cache_directory.c_str(),
      "--socket",
      socket_path,
      "--api-fd",
      api_descriptor.c_str(),
      "--width",
      width.c_str(),
      "--height",
      height.c_str(),
      nullptr,
  };
  const pid_t child = fork();
  if (child == 0) {
    close(host_api_fd);
    // OOS favors steady-state application performance over startup latency:
    // compile every Wasm function with BBQ before exposing the instance.
    if (!std::getenv("JSC_useEagerBBQCompilation"))
      setenv("JSC_useEagerBBQCompilation", "true", 1);
    if (!std::getenv("JSC_useWasmIPInt"))
      setenv("JSC_useWasmIPInt", "false", 1);
    if (!std::getenv("JSC_useWasmOSR"))
      setenv("JSC_useWasmOSR", "false", 1);
    const char *wpe_library_path = environmentOr(
        "OOS_WPE_LD_LIBRARY_PATH",
        "/opt/oos/lib:/system/lib:/vendor/lib:/apex/com.android.runtime/lib");
    setenv("LD_LIBRARY_PATH", wpe_library_path, 1);
    execv(runner, const_cast<char *const *>(arguments.data()));
    _exit(127);
  }
  return child;
}

} // namespace

WpeAppHost::WpeAppHost(compositor::Compositor &compositor,
                       input::KeyInputSource &input)
    : compositor_(compositor), input_(input) {}

bool WpeAppHost::run(const apps::AppLaunch &launch,
                     const device::DeviceDescriptor &device,
                     volatile std::sig_atomic_t *stop_requested) {
  error_.clear();
  const char *socket_path =
      environmentOr("OOS_WPE_SURFACE_SOCKET", "/data/runtime/wpe-surface.sock");
  const int listener = oos_surface_transport_listen(socket_path);
  if (listener < 0) {
    error_ = errorText("create WPE surface listener", listener);
    return false;
  }
  int api_sockets[2] = {-1, -1};
  const int api_pair_result = oos_device_api_socket_pair(api_sockets);
  if (api_pair_result != 0) {
    error_ = errorText("create WPE device API channel", api_pair_result);
    close(listener);
    unlink(socket_path);
    return false;
  }
  const pid_t child =
      startRunner(launch, device, socket_path, api_sockets[1], api_sockets[0]);
  if (child < 0) {
    error_ = std::string("start WPE runner: ") + std::strerror(errno);
    close(listener);
    close(api_sockets[0]);
    close(api_sockets[1]);
    unlink(socket_path);
    return false;
  }
  close(api_sockets[1]);

  int connection = -ETIMEDOUT;
  int child_status = 0;
  for (int waited = 0; waited < kAcceptTimeoutMs; waited += kAcceptSliceMs) {
    if ((stop_requested && *stop_requested) || input_.stopRequested() ||
        childExited(child, child_status))
      break;
    connection = oos_surface_transport_accept(listener, kAcceptSliceMs);
    if (connection >= 0 || connection != -ETIMEDOUT)
      break;
  }
  if (connection < 0) {
    const bool externally_stopped =
        (stop_requested && *stop_requested) || input_.stopRequested();
    error_ = errorText("accept WPE producer", connection);
    stopChild(child);
    close(listener);
    close(api_sockets[0]);
    unlink(socket_path);
    return externally_stopped;
  }

  bool success = true;
  uint64_t presented_frames = 0;
  KeyDispatchContext key_context{connection, true,
                                 std::getenv("OOS_TRACE_KEYS") != nullptr};
  storage::DeviceStorageService device_storage;
  bool device_api_connected = true;
  while (!(stop_requested && *stop_requested) && !input_.stopRequested()) {
    if (!serviceDeviceApi(api_sockets[0], device_storage, device_api_connected,
                          error_)) {
      success = false;
      break;
    }
    OosSurfaceTransportFrame packet = {};
    AHardwareBuffer *buffer = nullptr;
    const int received = oos_surface_transport_receive(connection, &packet,
                                                       &buffer, kFramePollMs);
    if (received == 0)
      break;
    if (received < 0 && received != -ETIMEDOUT) {
      error_ = errorText("receive WPE frame", received);
      success = false;
      break;
    }
    if (received > 0) {
      AHardwareBuffer_Desc description = {};
      AHardwareBuffer_describe(buffer, &description);
      compositor::SurfaceFrame frame;
      frame.surface_id = packet.surface_id;
      frame.buffer = buffer;
      frame.buffer_width = description.width;
      frame.buffer_height = description.height;
      frame.buffer_stride = description.stride;
      frame.width = packet.width;
      frame.height = packet.height;
      const bool presented = compositor_.presentSurface(frame);
      AHardwareBuffer_release(buffer);
      const int acknowledged =
          oos_surface_transport_acknowledge(connection, presented);
      if (!presented || acknowledged != 0) {
        error_ = !presented ? "OOS compositor rejected the WPE surface"
                            : errorText("acknowledge WPE frame", acknowledged);
        success = false;
        break;
      }
      ++presented_frames;
    }
    if (input_.poll(0, sendKey, &key_context) < 0 || !key_context.success) {
      error_ = "forward key input to WPE failed";
      success = false;
      break;
    }
    if (!serviceDeviceApi(api_sockets[0], device_storage, device_api_connected,
                          error_)) {
      success = false;
      break;
    }
    if (childExited(child, child_status))
      break;
  }

  close(connection);
  close(api_sockets[0]);
  close(listener);
  unlink(socket_path);
  const bool externally_stopped =
      (stop_requested && *stop_requested) || input_.stopRequested();
  if (!childExited(child, child_status))
    stopChild(child);
  else if (!externally_stopped &&
           (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)) {
    error_ = "WPE runner exited unsuccessfully";
    success = false;
  }
  std::fprintf(stderr, "OOS WPE host presented %llu frames\n",
               static_cast<unsigned long long>(presented_frames));
  return success && (presented_frames > 0 || externally_stopped);
}

} // namespace oos::web
