#include "oos/web/wpe_app_host.h"

#include "oos/apps/permissions.h"
#include "oos/compositor/compositor.h"
#include "oos/compositor/surface_transport.h"
#include "oos/device/device.h"
#include "oos/device/service_provider.h"
#include "oos/input/key_input.h"
#include "oos/services/system_service.h"
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
#include <unordered_map>
#include <vector>

extern "C" {
void AHardwareBuffer_describe(const AHardwareBuffer *buffer,
                              AHardwareBuffer_Desc *description);
void AHardwareBuffer_release(AHardwareBuffer *buffer);
}

namespace oos::web {
namespace {

constexpr int kFramePollMs = 16;
constexpr int kChildStopSlices = 50;
constexpr useconds_t kChildStopSliceUs = 20000;

const char *environmentOr(const char *name, const char *fallback) {
  const char *value = std::getenv(name);
  return value && value[0] ? value : fallback;
}

bool environmentEnabled(const char *name) {
  const char *value = std::getenv(name);
  return value && value[0] && std::strcmp(value, "0") != 0;
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
  int result = 0;
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
  if (result != 0 && context->result == 0)
    context->result = result;
}

pid_t startRunner(const apps::AppLaunch &launch,
                  const device::DeviceDescriptor &device, int surface_fd,
                  int host_surface_fd, int input_fd, int host_input_fd,
                  int api_fd, int host_api_fd) {
  const char *runner = environmentOr("OOS_WPE_RUNNER", "/opt/oos/bin/oos-wpe");
  const std::string width = std::to_string(device.primary_width);
  const std::string height = std::to_string(device.primary_height);
  const std::string surface_descriptor = std::to_string(surface_fd);
  const std::string input_descriptor = std::to_string(input_fd);
  const std::string api_descriptor = std::to_string(api_fd);
  std::vector<std::string> argument_storage = {
      runner,
      "--id",
      launch.app.manifest.id,
      "--package",
      launch.executable_path,
      "--entrypoint",
      launch.entrypoint,
      "--api-profile",
      launch.app.manifest.api_profile,
      "--data",
      launch.data_directory,
      "--cache",
      launch.cache_directory,
      "--surface-fd",
      surface_descriptor,
      "--input-fd",
      input_descriptor,
      "--api-fd",
      api_descriptor,
      "--width",
      width,
      "--height",
      height,
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
    close(host_surface_fd);
    close(host_input_fd);
    close(host_api_fd);
    // ARMv7 BBQ remains enabled, but its loop OSR entrypoint is not reliable on
    // the KaiOS JSC port. Hot functions still tier from IPInt to BBQ.
    setenv("JSC_useWasmOSR", "false", 1);
    if (environmentEnabled("OOS_ENABLE_INSPECTOR")) {
      setenv("WEBKIT_INSPECTOR_HTTP_SERVER",
             environmentOr("OOS_INSPECTOR_ADDRESS", "127.0.0.1:9222"), 1);
    }
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
                       input::KeyInputSource &input, device::Device &device,
                       apps::AppRepository &repository)
    : compositor_(compositor), input_(input), device_(device),
      repository_(repository),
      services_(std::make_unique<device::ServiceProvider>(device)) {}

WpeAppHost::~WpeAppHost() = default;

bool WpeAppHost::run(const apps::AppLaunch &launch,
                     volatile std::sig_atomic_t *stop_requested) {
  error_.clear();
  services::SystemServiceHub system_services(repository_.dataRoot(),
                                             &repository_);
  if (!system_services.initialize()) {
    error_ = "initialize OOS system services: " + system_services.lastError();
    return false;
  }
  const std::vector<apps::DataStoreGrant> data_store_grants =
      apps::ownedDataStoreGrants(launch.app.manifest.requested_permissions);
  std::unique_ptr<storage::AppStorage> app_storage;
  if (!data_store_grants.empty()) {
    app_storage = std::make_unique<storage::AppStorage>(launch.data_directory +
                                                        "/oos-platform");
    if (!app_storage->initialize()) {
      error_ = "initialize KaiOS DataStore: " + app_storage->lastError();
      return false;
    }
  }
  int surface_sockets[2] = {-1, -1};
  const int surface_pair_result =
      oos_surface_transport_socket_pair(surface_sockets);
  if (surface_pair_result != 0) {
    error_ = errorText("create WPE surface channel", surface_pair_result);
    return false;
  }
  int input_sockets[2] = {-1, -1};
  const int input_pair_result =
      oos_surface_transport_socket_pair(input_sockets);
  if (input_pair_result != 0) {
    error_ = errorText("create WPE input channel", input_pair_result);
    close(surface_sockets[0]);
    close(surface_sockets[1]);
    return false;
  }
  int api_sockets[2] = {-1, -1};
  const int api_pair_result = oos_device_api_socket_pair(api_sockets);
  if (api_pair_result != 0) {
    error_ = errorText("create WPE device API channel", api_pair_result);
    close(surface_sockets[0]);
    close(surface_sockets[1]);
    close(input_sockets[0]);
    close(input_sockets[1]);
    return false;
  }
  const pid_t child = startRunner(
      launch, device_.descriptor(), surface_sockets[1], surface_sockets[0],
      input_sockets[1], input_sockets[0], api_sockets[1], api_sockets[0]);
  if (child < 0) {
    error_ = std::string("start WPE runner: ") + std::strerror(errno);
    close(surface_sockets[0]);
    close(surface_sockets[1]);
    close(input_sockets[0]);
    close(input_sockets[1]);
    close(api_sockets[0]);
    close(api_sockets[1]);
    return false;
  }
  close(surface_sockets[1]);
  close(input_sockets[1]);
  close(api_sockets[1]);

  storage::DeviceStorageService device_storage;
  DeviceApiContext device_api_context;
  device_api_context.services = services_.get();
  device_api_context.device = &device_;
  device_api_context.app_storage = app_storage.get();
  device_api_context.system_services = &system_services;
  device_api_context.app_id = launch.app.manifest.id;
  device_api_context.permissions = launch.app.manifest.requested_permissions;
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

  int child_status = 0;
  bool success = true;
  uint64_t presented_frames = 0;
  std::unordered_map<uint64_t, AHardwareBuffer *> surface_buffers;
  const bool trace_frames = environmentEnabled("OOS_TRACE_WPE_FRAMES");
  KeyDispatchContext key_context{input_sockets[0], 0,
                                 std::getenv("OOS_TRACE_KEYS") != nullptr};
  while (!(stop_requested && *stop_requested) && !input_.stopRequested()) {
    if (!device_api_success) {
      error_ = device_api_error;
      success = false;
      break;
    }
    OosSurfaceTransportFrame packet = {};
    AHardwareBuffer *buffer = nullptr;
    const int received = oos_surface_transport_receive(
        surface_sockets[0], &packet, &buffer, kFramePollMs);
    if (received == 0)
      break;
    if (received < 0 && received != -ETIMEDOUT) {
      error_ = errorText("receive WPE frame", received);
      success = false;
      break;
    }
    if (received > 0) {
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
        error_ = "WPE frame referenced an unknown surface buffer";
        success = false;
        break;
      }
      buffer = buffer_entry->second;
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
      const int acknowledged =
          oos_surface_transport_acknowledge(surface_sockets[0], presented);
      if (!presented || acknowledged != 0) {
        error_ = !presented ? "OOS compositor rejected the WPE surface"
                            : errorText("acknowledge WPE frame", acknowledged);
        success = false;
        break;
      }
      ++presented_frames;
      if (trace_frames) {
        std::fprintf(stderr, "OOS WPE host presented frame=%llu\n",
                     static_cast<unsigned long long>(presented_frames));
        std::fflush(stderr);
      }
    }
    if (input_.poll(0, sendKey, &key_context) < 0 || key_context.result != 0) {
      error_ = key_context.result
                   ? errorText("forward key input to WPE", key_context.result)
                   : "poll key input for WPE failed";
      success = false;
      break;
    }
    if (childExited(child, child_status))
      break;
  }

  close(surface_sockets[0]);
  close(input_sockets[0]);
  for (const auto &[buffer_id, retained_buffer] : surface_buffers) {
    (void)buffer_id;
    AHardwareBuffer_release(retained_buffer);
  }
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
