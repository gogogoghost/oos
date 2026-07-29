#include "oos/web/wpe_app_host.h"

#include "oos/compositor/compositor.h"
#include "oos/compositor/surface_transport.h"
#include "oos/apps/permissions.h"
#include "oos/device/device.h"
#include "oos/device/service_provider.h"
#include "oos/input/key_input.h"
#include "oos/storage/app_storage.h"
#include "oos/storage/device_storage.h"
#include "oos/web/device_api_service.h"
#include "oos/web/device_api_transport.h"

#include <android/hardware_buffer.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
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

pid_t startRunner(const apps::AppLaunch &launch,
                  const device::DeviceDescriptor &device,
                  const char *socket_path, int api_fd, int host_api_fd) {
  const char *runner = environmentOr("OOS_WPE_RUNNER", "/opt/oos/bin/oos-wpe");
  const std::string width = std::to_string(device.primary_width);
  const std::string height = std::to_string(device.primary_height);
  const std::string api_descriptor = std::to_string(api_fd);
  std::vector<std::string> argument_storage = {
      runner,
      "--id", launch.app.manifest.id,
      "--package", launch.executable_path,
      "--entrypoint", launch.entrypoint,
      "--api-profile", launch.app.manifest.api_profile,
      "--data", launch.data_directory,
      "--cache", launch.cache_directory,
      "--socket", socket_path,
      "--api-fd", api_descriptor,
      "--width", width,
      "--height", height,
  };
  for (const std::string &permission :
       launch.app.manifest.requested_permissions) {
    argument_storage.emplace_back("--permission");
    argument_storage.push_back(permission);
  }
  std::vector<char *> arguments;
  arguments.reserve(argument_storage.size() + 1);
  for (std::string &argument : argument_storage)
    arguments.push_back(argument.data());
  arguments.push_back(nullptr);
  const pid_t child = fork();
  if (child == 0) {
    close(host_api_fd);
    const char *wpe_library_path = environmentOr(
        "OOS_WPE_LD_LIBRARY_PATH",
        "/opt/oos/lib:/system/lib:/vendor/lib:/apex/com.android.runtime/lib");
    setenv("LD_LIBRARY_PATH", wpe_library_path, 1);
    execv(runner, arguments.data());
    _exit(127);
  }
  return child;
}

} // namespace

WpeAppHost::WpeAppHost(compositor::Compositor &compositor,
                       input::KeyInputSource &input, device::Device &device)
    : compositor_(compositor), input_(input), device_(device),
      services_(std::make_unique<device::ServiceProvider>(device)) {}

WpeAppHost::~WpeAppHost() = default;

bool WpeAppHost::run(const apps::AppLaunch &launch,
                     volatile std::sig_atomic_t *stop_requested) {
  error_.clear();
  const std::vector<apps::DataStoreGrant> data_store_grants =
      apps::ownedDataStoreGrants(launch.app.manifest.requested_permissions);
  std::unique_ptr<storage::AppStorage> app_storage;
  if (!data_store_grants.empty()) {
    app_storage = std::make_unique<storage::AppStorage>(
        launch.data_directory + "/oos-platform");
    if (!app_storage->initialize()) {
      error_ = "initialize KaiOS DataStore: " + app_storage->lastError();
      return false;
    }
  }
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
  const pid_t child = startRunner(launch, device_.descriptor(), socket_path,
                                  api_sockets[1], api_sockets[0]);
  if (child < 0) {
    error_ = std::string("start WPE runner: ") + std::strerror(errno);
    close(listener);
    close(api_sockets[0]);
    close(api_sockets[1]);
    unlink(socket_path);
    return false;
  }
  close(api_sockets[1]);

  storage::DeviceStorageService device_storage;
  DeviceApiContext device_api_context;
  device_api_context.services = services_.get();
  device_api_context.device = &device_;
  device_api_context.app_storage = app_storage.get();
  device_api_context.permission_mask = apps::deviceServicePermissionMask(
      launch.app.manifest.requested_permissions);
  for (const apps::DataStoreGrant &grant : data_store_grants)
    device_api_context.owned_data_stores.emplace(grant.name, grant.writable);
  std::atomic<bool> stop_device_api{false};
  std::atomic<bool> device_api_success{true};
  bool device_api_connected = true;
  std::string device_api_error;
  std::thread device_api_thread([&] {
    while (!stop_device_api && device_api_connected) {
      if (!serviceDeviceApi(api_sockets[0], device_storage,
                            device_api_connected, device_api_error, 50,
                            &device_api_context)) {
        device_api_success = false;
        break;
      }
    }
  });

  const auto stopDeviceApi = [&] {
    stop_device_api = true;
    if (device_api_thread.joinable())
      device_api_thread.join();
    close(api_sockets[0]);
    for (const std::string &wake_lock : device_api_context.wake_locks)
      services_->releaseWakeLock(wake_lock);
    device_api_context.wake_locks.clear();
  };

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
    stopDeviceApi();
    close(listener);
    unlink(socket_path);
    return externally_stopped;
  }

  bool success = true;
  uint64_t presented_frames = 0;
  KeyDispatchContext key_context{connection, true,
                                 std::getenv("OOS_TRACE_KEYS") != nullptr};
  while (!(stop_requested && *stop_requested) && !input_.stopRequested()) {
    if (!device_api_success) {
      error_ = device_api_error;
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
    if (childExited(child, child_status))
      break;
  }

  close(connection);
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
  stopDeviceApi();
  if (!device_api_success) {
    error_ = device_api_error;
    success = false;
  }
  std::fprintf(stderr, "OOS WPE host presented %llu frames\n",
               static_cast<unsigned long long>(presented_frames));
  return success && (presented_frames > 0 || externally_stopped);
}

} // namespace oos::web
