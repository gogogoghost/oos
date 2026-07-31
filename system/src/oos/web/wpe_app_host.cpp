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

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
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
constexpr size_t kMaxQueuedFrames = 2;
constexpr useconds_t kChildStopSliceUs = 20000;
constexpr int64_t kMetricsReportIntervalNs = 5000000000LL;

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

int64_t monotonicTimeNs() {
  timespec time{};
  return clock_gettime(CLOCK_MONOTONIC, &time) == 0
             ? static_cast<int64_t>(time.tv_sec) * 1000000000LL + time.tv_nsec
             : 0;
}

class FrameMetrics {
public:
  explicit FrameMetrics(bool enabled) : enabled_(enabled) {
    queue_us_.reserve(512);
    present_us_.reserve(512);
    total_us_.reserve(512);
  }

  void record(int64_t submitted_ns, int64_t queued_ns, int64_t started_ns,
              int64_t finished_ns, bool presented) {
    if (!enabled_)
      return;
    std::lock_guard lock(mutex_);
    queue_us_.push_back((started_ns - queued_ns) / 1000);
    present_us_.push_back((finished_ns - started_ns) / 1000);
    total_us_.push_back((finished_ns - submitted_ns) / 1000);
    presented ? ++presented_ : ++dropped_;
    if (!last_report_ns_)
      last_report_ns_ = finished_ns;
    if (finished_ns - last_report_ns_ >= kMetricsReportIntervalNs)
      report(finished_ns);
  }

  void recordDropped() {
    if (!enabled_)
      return;
    std::lock_guard lock(mutex_);
    ++dropped_;
  }

  void finish() {
    std::lock_guard lock(mutex_);
    if (enabled_ && (!queue_us_.empty() || dropped_))
      report(monotonicTimeNs());
  }

private:
  static int64_t percentile(const std::vector<int64_t> &samples,
                            size_t percent) {
    if (samples.empty())
      return 0;
    std::vector<int64_t> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const size_t index = (sorted.size() - 1) * percent / 100;
    return sorted[index];
  }

  static void printDistribution(const char *name,
                                const std::vector<int64_t> &samples) {
    std::fprintf(stderr, " %s_us[p50=%lld p95=%lld p99=%lld]", name,
                 static_cast<long long>(percentile(samples, 50)),
                 static_cast<long long>(percentile(samples, 95)),
                 static_cast<long long>(percentile(samples, 99)));
  }

  void report(int64_t now_ns) {
    std::fprintf(stderr, "OOS WPE frame profile: presented=%llu dropped=%llu",
                 static_cast<unsigned long long>(presented_),
                 static_cast<unsigned long long>(dropped_));
    printDistribution("queue", queue_us_);
    printDistribution("present", present_us_);
    printDistribution("total", total_us_);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    queue_us_.clear();
    present_us_.clear();
    total_us_.clear();
    presented_ = 0;
    dropped_ = 0;
    last_report_ns_ = now_ns;
  }

  bool enabled_ = false;
  std::mutex mutex_;
  int64_t last_report_ns_ = 0;
  uint64_t presented_ = 0;
  uint64_t dropped_ = 0;
  std::vector<int64_t> queue_us_;
  std::vector<int64_t> present_us_;
  std::vector<int64_t> total_us_;
};

class SurfacePresentationQueue {
public:
  SurfacePresentationQueue(compositor::Compositor &compositor, int socket_fd,
                           bool trace_frames, bool profile_frames)
      : compositor_(compositor), socket_fd_(socket_fd),
        trace_frames_(trace_frames), metrics_(profile_frames) {}

  ~SurfacePresentationQueue() { stop(); }

  bool start() {
    if (!compositor_.detachRenderContext())
      return false;
    context_transferred_ = true;
    worker_ = std::thread([this] { run(); });
    std::unique_lock lock(start_mutex_);
    start_ready_.wait(lock, [this] { return start_complete_; });
    if (start_success_)
      return true;
    lock.unlock();
    if (worker_.joinable())
      worker_.join();
    context_transferred_ = false;
    compositor_.attachRenderContext();
    return false;
  }

  bool enqueue(const compositor::SurfaceFrame &frame, int64_t submitted_ns) {
    QueuedFrame dropped;
    bool has_dropped = false;
    {
      std::lock_guard lock(queue_mutex_);
      if (stopping_) {
        if (frame.acquire_fence_fd >= 0)
          close(frame.acquire_fence_fd);
        return false;
      }
      if (queue_.size() >= kMaxQueuedFrames) {
        dropped = queue_.front();
        queue_.pop_front();
        has_dropped = true;
      }
      queue_.push_back({frame, submitted_ns, monotonicTimeNs()});
    }
    if (has_dropped) {
      if (dropped.frame.acquire_fence_fd >= 0)
        close(dropped.frame.acquire_fence_fd);
      metrics_.recordDropped();
      sendRelease(dropped.frame.sequence, false);
    }
    queue_ready_.notify_one();
    return healthy_;
  }

  void stop() {
    std::deque<QueuedFrame> canceled;
    {
      std::lock_guard lock(queue_mutex_);
      if (stopping_ && !worker_.joinable())
        return;
      stopping_ = true;
      canceled.swap(queue_);
    }
    for (const QueuedFrame &frame : canceled) {
      if (frame.frame.acquire_fence_fd >= 0)
        close(frame.frame.acquire_fence_fd);
      metrics_.recordDropped();
      sendRelease(frame.frame.sequence, false);
    }
    queue_ready_.notify_all();
    if (worker_.joinable())
      worker_.join();
    if (context_transferred_) {
      if (!compositor_.attachRenderContext())
        healthy_ = false;
      context_transferred_ = false;
    }
    metrics_.finish();
  }

  bool healthy() const { return healthy_; }
  uint64_t presentedFrames() const { return presented_frames_; }

private:
  struct QueuedFrame {
    compositor::SurfaceFrame frame;
    int64_t submitted_ns = 0;
    int64_t queued_ns = 0;
  };

  void run() {
    const bool attached = compositor_.attachRenderContext();
    {
      std::lock_guard lock(start_mutex_);
      start_success_ = attached;
      start_complete_ = true;
    }
    start_ready_.notify_one();
    if (!attached)
      return;
    while (true) {
      QueuedFrame queued;
      {
        std::unique_lock lock(queue_mutex_);
        queue_ready_.wait(lock,
                          [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) {
          if (stopping_)
            break;
          continue;
        }
        queued = queue_.front();
        queue_.pop_front();
      }
      const int64_t started_ns = monotonicTimeNs();
      const bool presented = compositor_.presentSurface(queued.frame);
      const int64_t finished_ns = monotonicTimeNs();
      metrics_.record(queued.submitted_ns, queued.queued_ns, started_ns,
                      finished_ns, presented);
      if (presented)
        ++presented_frames_;
      if (!sendRelease(queued.frame.sequence, presented) || !presented)
        healthy_ = false;
      if (trace_frames_) {
        std::fprintf(stderr,
                     "OOS WPE host completed sequence=%llu presented=%d\n",
                     static_cast<unsigned long long>(queued.frame.sequence),
                     presented ? 1 : 0);
      }
    }
    if (!compositor_.detachRenderContext())
      healthy_ = false;
  }

  bool sendRelease(uint64_t sequence, bool accepted) {
    const OosSurfaceTransportRelease release = {
        .sequence = sequence,
        .presented_at_ns = monotonicTimeNs(),
        .accepted = static_cast<uint8_t>(accepted),
        .reserved = {},
    };
    std::lock_guard lock(send_mutex_);
    return oos_surface_transport_release(socket_fd_, &release) == 0;
  }

  compositor::Compositor &compositor_;
  int socket_fd_ = -1;
  bool trace_frames_ = false;
  FrameMetrics metrics_;
  std::mutex queue_mutex_;
  std::mutex send_mutex_;
  std::mutex start_mutex_;
  std::condition_variable queue_ready_;
  std::condition_variable start_ready_;
  std::deque<QueuedFrame> queue_;
  std::thread worker_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> healthy_{true};
  std::atomic<uint64_t> presented_frames_{0};
  bool context_transferred_ = false;
  bool start_complete_ = false;
  bool start_success_ = false;
};

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
      "--package-kind",
      apps::packageKindName(launch.app.manifest.package_kind),
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
  std::unordered_map<uint64_t, AHardwareBuffer *> surface_buffers;
  const bool trace_frames = environmentEnabled("OOS_TRACE_WPE_FRAMES");
  SurfacePresentationQueue presentation_queue(
      compositor_, surface_sockets[0], trace_frames,
      environmentEnabled("OOS_PROFILE_WPE_FRAMES"));
  if (!presentation_queue.start()) {
    error_ = "transfer display context to WPE presentation thread failed";
    success = false;
  }
  KeyDispatchContext key_context{input_sockets[0], 0,
                                 std::getenv("OOS_TRACE_KEYS") != nullptr};
  while (success && !(stop_requested && *stop_requested) &&
         !input_.stopRequested()) {
    if (!device_api_success) {
      error_ = device_api_error;
      success = false;
      break;
    }
    if (!presentation_queue.healthy()) {
      error_ = "WPE surface presentation queue failed";
      success = false;
      break;
    }
    OosSurfaceTransportFrame packet = {};
    AHardwareBuffer *buffer = nullptr;
    int acquire_fence_fd = -1;
    const int received = oos_surface_transport_receive(
        surface_sockets[0], &packet, &buffer, &acquire_fence_fd, kFramePollMs);
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
        if (acquire_fence_fd >= 0)
          close(acquire_fence_fd);
        error_ = "WPE frame referenced an unknown surface buffer";
        success = false;
        break;
      }
      buffer = buffer_entry->second;
      AHardwareBuffer_Desc description = {};
      AHardwareBuffer_describe(buffer, &description);
      compositor::SurfaceFrame frame;
      frame.surface_id = packet.surface_id;
      frame.sequence = packet.sequence;
      frame.buffer = buffer;
      frame.buffer_width = description.width;
      frame.buffer_height = description.height;
      frame.buffer_stride = description.stride;
      frame.acquire_fence_fd = acquire_fence_fd;
      frame.width = packet.width;
      frame.height = packet.height;
      if (!presentation_queue.enqueue(frame, packet.submitted_at_ns)) {
        error_ = "enqueue WPE surface frame failed";
        success = false;
        break;
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

  presentation_queue.stop();
  const uint64_t presented_frames = presentation_queue.presentedFrames();
  if (!presentation_queue.healthy() && success) {
    error_ = "WPE surface presentation queue failed";
    success = false;
  }
  const bool externally_stopped =
      (stop_requested && *stop_requested) || input_.stopRequested();
  // Keep all service channels alive while the runner tears down WebKit. Closing
  // them first makes both the GLib HUP sources and SIGTERM initiate shutdown at
  // once, which is unsafe in the Android WPE backend during view destruction.
  if (!childExited(child, child_status))
    stopChild(child);
  else if (!externally_stopped &&
           (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)) {
    error_ = "WPE runner exited unsuccessfully";
    success = false;
  }
  close(surface_sockets[0]);
  close(input_sockets[0]);
  for (const auto &[buffer_id, retained_buffer] : surface_buffers) {
    (void)buffer_id;
    AHardwareBuffer_release(retained_buffer);
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
