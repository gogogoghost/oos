#include "oos/hardware/camera_manager.h"

#include <hardware/camera.h>
#include <hardware/hardware.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace oos::hardware {
namespace {

constexpr const char *kTorchPath = "/sys/class/leds/torch-light0/brightness";

struct OwnedMemory {
  camera_memory_t memory{};
  void *base = nullptr;
  size_t buffer_size = 0;
  size_t total_size = 0;
  int fd = -1;
  bool mapped = false;
};

struct CaptureState {
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<uint8_t> jpeg;
  bool failed = false;
};

void releaseMemory(camera_memory_t *memory) {
  auto *owned = static_cast<OwnedMemory *>(memory->handle);
  if (owned->mapped)
    munmap(owned->base, owned->total_size);
  else
    std::free(owned->base);
  if (owned->fd >= 0)
    close(owned->fd);
  delete owned;
}

camera_memory_t *requestMemory(int fd, size_t buffer_size, unsigned num_buffers,
                               void *) {
  if (buffer_size == 0 || num_buffers == 0 ||
      buffer_size > SIZE_MAX / num_buffers)
    return nullptr;
  auto *owned = new OwnedMemory();
  owned->buffer_size = buffer_size;
  owned->total_size = buffer_size * num_buffers;
  if (fd >= 0) {
    owned->fd = dup(fd);
    if (owned->fd >= 0) {
      owned->base = mmap(nullptr, owned->total_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, owned->fd, 0);
      owned->mapped = owned->base != MAP_FAILED;
    }
  } else {
    owned->base = std::calloc(1, owned->total_size);
  }
  if (!owned->base || owned->base == MAP_FAILED) {
    if (owned->fd >= 0)
      close(owned->fd);
    delete owned;
    return nullptr;
  }
  owned->memory.data = owned->base;
  owned->memory.size = owned->total_size;
  owned->memory.handle = owned;
  owned->memory.release = releaseMemory;
  return &owned->memory;
}

void notifyCallback(int32_t message, int32_t, int32_t, void *context) {
  if (message != CAMERA_MSG_ERROR)
    return;
  auto *state = static_cast<CaptureState *>(context);
  std::lock_guard<std::mutex> lock(state->mutex);
  state->failed = true;
  state->condition.notify_all();
}

void dataCallback(int32_t message, const camera_memory_t *memory,
                  unsigned index, camera_frame_metadata_t *, void *context) {
  if (message != CAMERA_MSG_COMPRESSED_IMAGE)
    return;
  auto *state = static_cast<CaptureState *>(context);
  auto *owned = memory ? static_cast<OwnedMemory *>(memory->handle) : nullptr;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (!owned || index * owned->buffer_size >= owned->total_size) {
    state->failed = true;
  } else {
    const auto *begin =
        static_cast<const uint8_t *>(owned->base) + index * owned->buffer_size;
    size_t length = owned->buffer_size;
    if (length >= 2 && begin[0] == 0xff && begin[1] == 0xd8) {
      for (size_t offset = length - 1; offset > 0; --offset) {
        if (begin[offset - 1] == 0xff && begin[offset] == 0xd9) {
          length = offset + 1;
          break;
        }
      }
    }
    state->jpeg.assign(begin, begin + length);
  }
  state->condition.notify_all();
}

void timestampCallback(int64_t, int32_t, const camera_memory_t *, unsigned,
                       void *) {}

std::string setParameter(std::string parameters, const std::string &name,
                         const std::string &value) {
  const std::string prefix = name + "=";
  const size_t start = parameters.find(prefix);
  if (start == std::string::npos) {
    if (!parameters.empty() && parameters.back() != ';')
      parameters += ';';
    return parameters + prefix + value;
  }
  const size_t end = parameters.find(';', start);
  parameters.replace(start,
                     end == std::string::npos ? std::string::npos : end - start,
                     prefix + value);
  return parameters;
}

std::string parameterValue(const std::string &parameters,
                           const std::string &name) {
  const std::string prefix = name + "=";
  const size_t start = parameters.find(prefix);
  if (start == std::string::npos)
    return {};
  const size_t value_start = start + prefix.size();
  const size_t end = parameters.find(';', value_start);
  return parameters.substr(value_start, end == std::string::npos
                                            ? std::string::npos
                                            : end - value_start);
}

std::vector<std::pair<int, int>> parseSizes(const std::string &value) {
  std::vector<std::pair<int, int>> sizes;
  size_t offset = 0;
  while (offset <= value.size()) {
    const size_t comma = value.find(',', offset);
    const std::string item =
        value.substr(offset, comma == std::string::npos ? std::string::npos
                                                        : comma - offset);
    const size_t separator = item.find('x');
    if (separator != std::string::npos) {
      const int width = std::atoi(item.substr(0, separator).c_str());
      const int height = std::atoi(item.substr(separator + 1).c_str());
      if (width > 0 && height > 0)
        sizes.emplace_back(width, height);
    }
    if (comma == std::string::npos)
      break;
    offset = comma + 1;
  }
  return sizes;
}

std::pair<int, int> selectSize(const std::vector<std::pair<int, int>> &sizes,
                               int max_width, int max_height) {
  std::pair<int, int> selected{};
  for (const auto &size : sizes) {
    if (size.first <= max_width && size.second <= max_height &&
        static_cast<int64_t>(size.first) * size.second >
            static_cast<int64_t>(selected.first) * selected.second)
      selected = size;
  }
  if (selected.first == 0 && !sizes.empty())
    selected = *std::min_element(
        sizes.begin(), sizes.end(), [](const auto &left, const auto &right) {
          return static_cast<int64_t>(left.first) * left.second <
                 static_cast<int64_t>(right.first) * right.second;
        });
  return selected;
}

bool listContains(const std::string &values, const std::string &needle) {
  size_t offset = 0;
  while (offset <= values.size()) {
    const size_t comma = values.find(',', offset);
    if (values.substr(offset, comma == std::string::npos
                                  ? std::string::npos
                                  : comma - offset) == needle)
      return true;
    if (comma == std::string::npos)
      break;
    offset = comma + 1;
  }
  return false;
}

std::string takeParameters(camera_device_t *camera) {
  char *raw = camera->ops->get_parameters(camera);
  std::string parameters = raw ? raw : "";
  if (raw) {
    if (camera->ops->put_parameters)
      camera->ops->put_parameters(camera, raw);
    else
      std::free(raw);
  }
  return parameters;
}

void closeCamera(camera_device_t *camera) {
  if (!camera)
    return;
  if (camera->ops->release)
    camera->ops->release(camera);
  camera->common.close(&camera->common);
}

bool writeText(const char *path, const char *value) {
  const int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  const size_t length = std::strlen(value);
  const bool success = write(fd, value, length) == static_cast<ssize_t>(length);
  const int saved_errno = errno;
  close(fd);
  errno = saved_errno;
  return success;
}

} // namespace

struct CameraManager::Implementation {
  camera_module_t *module = nullptr;
  std::string error;
};

CameraManager::CameraManager()
    : implementation_(std::make_unique<Implementation>()) {}

CameraManager::~CameraManager() { shutdown(); }

bool CameraManager::initialize() {
  shutdown();
  const hw_module_t *hardware = nullptr;
  const int load = hw_get_module(CAMERA_HARDWARE_MODULE_ID, &hardware);
  if (load != 0 || !hardware) {
    implementation_->error = "load camera HAL failed: " + std::to_string(load);
    return false;
  }
  implementation_->module =
      reinterpret_cast<camera_module_t *>(const_cast<hw_module_t *>(hardware));
  if (!implementation_->module->get_number_of_cameras ||
      !implementation_->module->get_camera_info ||
      !implementation_->module->common.methods ||
      !implementation_->module->common.methods->open) {
    implementation_->error = "camera HAL1 entry points are incomplete";
    implementation_->module = nullptr;
    return false;
  }
  implementation_->error.clear();
  return true;
}

void CameraManager::shutdown() {
  if (implementation_)
    implementation_->module = nullptr;
}

bool CameraManager::initialized() const {
  return implementation_ && implementation_->module;
}

bool CameraManager::enumerate(std::vector<CameraInfo> &cameras) {
  cameras.clear();
  if (!initialized()) {
    implementation_->error = "camera manager is not initialized";
    return false;
  }
  const int count = implementation_->module->get_number_of_cameras();
  if (count < 0) {
    implementation_->error = "camera HAL returned an invalid camera count";
    return false;
  }
  for (int index = 0; index < count; ++index) {
    camera_info native_info{};
    if (implementation_->module->get_camera_info(index, &native_info) != 0) {
      implementation_->error =
          "get camera info failed for " + std::to_string(index);
      return false;
    }
    CameraInfo info;
    info.id = std::to_string(index);
    info.facing = native_info.facing == CAMERA_FACING_BACK ? LensFacing::Back
                  : native_info.facing == CAMERA_FACING_FRONT
                      ? LensFacing::Front
                      : LensFacing::Unknown;
    info.sensor_orientation = native_info.orientation;
    info.hardware_level = 1;
    hw_device_t *hardware_device = nullptr;
    if (implementation_->module->common.methods->open(
            &implementation_->module->common, info.id.c_str(),
            &hardware_device) == 0 &&
        hardware_device) {
      auto *camera = reinterpret_cast<camera_device_t *>(hardware_device);
      const std::string parameters = takeParameters(camera);
      const auto sizes =
          parseSizes(parameterValue(parameters, "picture-size-values"));
      for (const auto &size : sizes) {
        if (static_cast<int64_t>(size.first) * size.second >
            static_cast<int64_t>(info.max_jpeg_width) * info.max_jpeg_height) {
          info.max_jpeg_width = size.first;
          info.max_jpeg_height = size.second;
        }
      }
      info.flash_available = listContains(
          parameterValue(parameters, "flash-mode-values"), "torch");
      closeCamera(camera);
    }
    cameras.push_back(std::move(info));
  }
  implementation_->error.clear();
  return true;
}

bool CameraManager::setTorch(const std::string &camera_id, bool enabled) {
  if (!initialized() || camera_id != "0") {
    implementation_->error = "invalid camera id for torch";
    return false;
  }
  if (!writeText(kTorchPath, enabled ? "255" : "0")) {
    implementation_->error =
        "write torch sysfs failed: " + std::string(std::strerror(errno));
    return false;
  }
  implementation_->error.clear();
  return true;
}

bool CameraManager::captureJpeg(const std::string &camera_id,
                                const std::string &path, PhotoResult &result,
                                int max_width, int max_height, bool flash,
                                int timeout_ms) {
  result = {};
  if (!initialized() || camera_id.empty() || path.empty() || max_width <= 0 ||
      max_height <= 0 || timeout_ms <= 0) {
    implementation_->error = "invalid camera capture parameters";
    return false;
  }
  hw_device_t *hardware_device = nullptr;
  const int open_result = implementation_->module->common.methods->open(
      &implementation_->module->common, camera_id.c_str(), &hardware_device);
  if (open_result != 0 || !hardware_device) {
    implementation_->error =
        "open camera failed: " + std::to_string(open_result);
    return false;
  }
  auto *camera = reinterpret_cast<camera_device_t *>(hardware_device);
  std::string parameters = takeParameters(camera);
  const auto size =
      selectSize(parseSizes(parameterValue(parameters, "picture-size-values")),
                 max_width, max_height);
  if (size.first == 0) {
    closeCamera(camera);
    implementation_->error = "camera reported no JPEG sizes";
    return false;
  }
  parameters = setParameter(std::move(parameters), "picture-size",
                            std::to_string(size.first) + "x" +
                                std::to_string(size.second));
  parameters = setParameter(std::move(parameters), "picture-format", "jpeg");
  parameters =
      setParameter(std::move(parameters), "flash-mode", flash ? "on" : "off");
  parameters = setParameter(std::move(parameters), "preview-size", "240x320");
  parameters = setParameter(std::move(parameters), "no-display-mode", "1");
  const int set_result =
      camera->ops->set_parameters(camera, parameters.c_str());
  if (set_result != 0) {
    closeCamera(camera);
    implementation_->error =
        "set camera parameters failed: " + std::to_string(set_result);
    return false;
  }
  CaptureState state;
  camera->ops->set_callbacks(camera, notifyCallback, dataCallback,
                             timestampCallback, requestMemory, &state);
  const int messages =
      CAMERA_MSG_ERROR | CAMERA_MSG_SHUTTER | CAMERA_MSG_COMPRESSED_IMAGE;
  camera->ops->enable_msg_type(camera, messages);
  int capture_result = camera->ops->start_preview(camera);
  if (capture_result == 0) {
    usleep(500000);
    capture_result = camera->ops->take_picture(camera);
  }
  bool completed = false;
  if (capture_result == 0) {
    std::unique_lock<std::mutex> lock(state.mutex);
    completed = state.condition.wait_for(
        lock, std::chrono::milliseconds(timeout_ms),
        [&] { return state.failed || !state.jpeg.empty(); });
  }
  if (!completed || state.failed || state.jpeg.empty())
    camera->ops->cancel_picture(camera);
  camera->ops->disable_msg_type(camera, messages);
  camera->ops->stop_preview(camera);
  bool wrote = false;
  if (completed && !state.failed && !state.jpeg.empty()) {
    FILE *file = std::fopen(path.c_str(), "wb");
    wrote = file && std::fwrite(state.jpeg.data(), 1, state.jpeg.size(),
                                file) == state.jpeg.size();
    if (file)
      wrote = std::fclose(file) == 0 && wrote;
  }
  closeCamera(camera);
  if (!wrote) {
    implementation_->error =
        capture_result != 0
            ? "take picture failed: " + std::to_string(capture_result)
            : "camera capture timed out or write failed";
    return false;
  }
  result.path = path;
  result.width = size.first;
  result.height = size.second;
  result.byte_count = state.jpeg.size();
  implementation_->error.clear();
  return true;
}

const std::string &CameraManager::lastError() const {
  return implementation_->error;
}

const char *lensFacingName(LensFacing facing) {
  switch (facing) {
  case LensFacing::Front:
    return "front";
  case LensFacing::Back:
    return "back";
  case LensFacing::External:
    return "external";
  case LensFacing::Unknown:
    return "unknown";
  }
  return "unknown";
}

const char *cameraHardwareLevelName(int level) {
  return level == 1 ? "legacy-hal1" : "unknown";
}

} // namespace oos::hardware
