#include "oos/hardware/camera_manager.h"

#include <android/hardware/camera/device/1.0/ICameraDevice.h>
#include <android/hardware/camera/provider/2.4/ICameraProvider.h>

#include <gui/BufferItemConsumer.h>
#include <gui/BufferQueue.h>
#include <gui/Surface.h>
#include <system/window.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace android::hardware {
void configureRpcThreadpool(size_t max_threads, bool caller_will_join);
} // namespace android::hardware

namespace oos::hardware {
namespace {

namespace camera_common = ::android::hardware::camera::common::V1_0;
namespace camera_device = ::android::hardware::camera::device::V1_0;
namespace camera_provider = ::android::hardware::camera::provider::V2_4;
namespace graphics_common = ::android::hardware::graphics::common::V1_0;
using ::android::BufferItem;
using ::android::BufferItemConsumer;
using ::android::BufferQueue;
using ::android::IGraphicBufferConsumer;
using ::android::IGraphicBufferProducer;
using ::android::sp;
using ::android::Surface;
using ::android::wp;
using ::android::hardware::hidl_handle;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;

camera_common::Status mapWindowStatus(android::status_t status) {
  switch (status) {
  case android::OK:
    return camera_common::Status::OK;
  case android::BAD_VALUE:
    return camera_common::Status::ILLEGAL_ARGUMENT;
  case -EBUSY:
    return camera_common::Status::CAMERA_IN_USE;
  case -EUSERS:
    return camera_common::Status::MAX_CAMERAS_IN_USE;
  case android::UNKNOWN_TRANSACTION:
    return camera_common::Status::METHOD_NOT_SUPPORTED;
  case android::INVALID_OPERATION:
    return camera_common::Status::OPERATION_NOT_SUPPORTED;
  case android::DEAD_OBJECT:
    return camera_common::Status::CAMERA_DISCONNECTED;
  default:
    return camera_common::Status::OPERATION_NOT_SUPPORTED;
  }
}

std::string statusError(const char *operation, camera_common::Status status) {
  return std::string(operation) +
         " failed (status=" + std::to_string(static_cast<int>(status)) + ")";
}

bool resultStatus(const Return<camera_common::Status> &result,
                  camera_common::Status &status) {
  if (!result.isOk())
    return false;
  status = static_cast<camera_common::Status>(result);
  return true;
}

std::string cameraIdFromName(const std::string &name) {
  const size_t slash = name.rfind('/');
  return slash == std::string::npos ? std::string() : name.substr(slash + 1);
}

bool isHal1Name(const std::string &name) {
  return name.rfind("device@1.0/", 0) == 0;
}

using Parameters = std::map<std::string, std::string>;

Parameters parseParameters(const std::string &serialized) {
  Parameters parameters;
  size_t offset = 0;
  while (offset <= serialized.size()) {
    const size_t end = serialized.find(';', offset);
    const size_t length =
        end == std::string::npos ? std::string::npos : end - offset;
    const std::string entry = serialized.substr(offset, length);
    const size_t equals = entry.find('=');
    if (equals != std::string::npos && equals != 0)
      parameters[entry.substr(0, equals)] = entry.substr(equals + 1);
    if (end == std::string::npos)
      break;
    offset = end + 1;
  }
  return parameters;
}

std::string serializeParameters(const Parameters &parameters) {
  std::string serialized;
  for (const auto &parameter : parameters) {
    if (!serialized.empty())
      serialized += ';';
    serialized += parameter.first + '=' + parameter.second;
  }
  return serialized;
}

std::vector<std::pair<int, int>> parseSizes(const std::string &serialized) {
  std::vector<std::pair<int, int>> sizes;
  size_t offset = 0;
  while (offset <= serialized.size()) {
    const size_t comma = serialized.find(',', offset);
    const std::string item =
        serialized.substr(offset, comma == std::string::npos ? std::string::npos
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
    if (size.first > max_width || size.second > max_height)
      continue;
    if (static_cast<int64_t>(size.first) * size.second >
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

bool valueListContains(const std::string &values, const std::string &needle) {
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

struct CaptureState {
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<uint8_t> jpeg;
  bool callback_error = false;
};

class CameraCallback final : public camera_device::ICameraDeviceCallback {
public:
  explicit CameraCallback(CaptureState *state) : state_(state) {}

  ~CameraCallback() override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &pool : pools_)
      destroyPool(pool.second);
  }

  Return<void> notifyCallback(camera_device::NotifyCallbackMsg message, int32_t,
                              int32_t) override {
    if (message == camera_device::NotifyCallbackMsg::ERROR && state_) {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->callback_error = true;
      state_->condition.notify_all();
    }
    return Void();
  }

  Return<uint32_t> registerMemory(const hidl_handle &descriptor,
                                  uint32_t buffer_size,
                                  uint32_t buffer_count) override {
    if (!descriptor.getNativeHandle() || descriptor->numFds != 1 ||
        descriptor->data[0] < 0 || buffer_size == 0 || buffer_count == 0)
      return 0;
    const size_t size = static_cast<size_t>(buffer_size) * buffer_count;
    if (size / buffer_count != buffer_size)
      return 0;
    const int fd = dup(descriptor->data[0]);
    if (fd < 0)
      return 0;
    void *address = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (address == MAP_FAILED) {
      close(fd);
      return 0;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t id = next_pool_id_++;
    pools_.emplace(id,
                   MemoryPool{fd, address, size, buffer_size, buffer_count});
    return id;
  }

  Return<void> unregisterMemory(uint32_t memory_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto pool = pools_.find(memory_id);
    if (pool != pools_.end()) {
      destroyPool(pool->second);
      pools_.erase(pool);
    }
    return Void();
  }

  Return<void>
  dataCallback(camera_device::DataCallbackMsg message, uint32_t memory_id,
               uint32_t buffer_index,
               const camera_device::CameraFrameMetadata &) override {
    if (message != camera_device::DataCallbackMsg::COMPRESSED_IMAGE || !state_)
      return Void();
    std::vector<uint8_t> jpeg;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto pool = pools_.find(memory_id);
      if (pool == pools_.end() || buffer_index >= pool->second.buffer_count) {
        std::lock_guard<std::mutex> state_lock(state_->mutex);
        state_->callback_error = true;
        state_->condition.notify_all();
        return Void();
      }
      const auto *begin =
          static_cast<const uint8_t *>(pool->second.address) +
          static_cast<size_t>(buffer_index) * pool->second.buffer_size;
      size_t length = pool->second.buffer_size;
      if (length >= 4 && begin[0] == 0xff && begin[1] == 0xd8) {
        for (size_t index = length - 1; index > 0; --index) {
          if (begin[index - 1] == 0xff && begin[index] == 0xd9) {
            length = index + 1;
            break;
          }
        }
      }
      jpeg.assign(begin, begin + length);
    }
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->jpeg = std::move(jpeg);
    }
    state_->condition.notify_all();
    return Void();
  }

  Return<void> dataCallbackTimestamp(camera_device::DataCallbackMsg, uint32_t,
                                     uint32_t, int64_t) override {
    return Void();
  }

  Return<void> handleCallbackTimestamp(camera_device::DataCallbackMsg,
                                       const hidl_handle &, uint32_t, uint32_t,
                                       int64_t) override {
    return Void();
  }

  Return<void> handleCallbackTimestampBatch(
      camera_device::DataCallbackMsg,
      const hidl_vec<camera_device::HandleTimestampMessage> &) override {
    return Void();
  }

private:
  struct MemoryPool {
    int fd;
    void *address;
    size_t size;
    uint32_t buffer_size;
    uint32_t buffer_count;
  };

  static void destroyPool(MemoryPool &pool) {
    munmap(pool.address, pool.size);
    close(pool.fd);
  }

  CaptureState *state_;
  std::mutex mutex_;
  std::unordered_map<uint32_t, MemoryPool> pools_;
  uint32_t next_pool_id_ = 1;
};

class FrameDrainer final : public BufferItemConsumer::FrameAvailableListener {
public:
  explicit FrameDrainer(const wp<BufferItemConsumer> &consumer)
      : consumer_(consumer) {}

  void onFrameAvailable(const BufferItem &) override {
    const sp<BufferItemConsumer> consumer = consumer_.promote();
    if (!consumer)
      return;
    BufferItem item;
    if (consumer->acquireBuffer(&item, 0) == android::OK) {
      consumer->releaseBuffer(item);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++frame_count_;
      }
      condition_.notify_all();
    }
  }

  bool waitForFrame(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                               [&] { return frame_count_ > 0; });
  }

private:
  wp<BufferItemConsumer> consumer_;
  std::mutex mutex_;
  std::condition_variable condition_;
  unsigned frame_count_ = 0;
};

class PreviewWindow final : public camera_device::ICameraDevicePreviewCallback {
public:
  explicit PreviewWindow(const sp<ANativeWindow> &window) : window_(window) {}

  Return<void> dequeueBuffer(dequeueBuffer_cb callback) override {
    ANativeWindowBuffer *buffer = nullptr;
    const android::status_t status =
        native_window_dequeue_buffer_and_wait(window_.get(), &buffer);
    uint64_t id = 0;
    uint32_t stride = 0;
    hidl_handle handle = nullptr;
    if (status == android::OK) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto found = ids_.find(buffer->handle);
      if (found == ids_.end()) {
        id = next_id_++;
        ids_[buffer->handle] = id;
        buffers_[id] = buffer;
        handle = buffer->handle;
      } else {
        id = found->second;
      }
      stride = buffer->stride;
    }
    callback(mapWindowStatus(status), id, handle, stride);
    return Void();
  }

  Return<camera_common::Status> enqueueBuffer(uint64_t id) override {
    ANativeWindowBuffer *buffer = findBuffer(id);
    return buffer ? mapWindowStatus(
                        window_->queueBuffer(window_.get(), buffer, -1))
                  : camera_common::Status::ILLEGAL_ARGUMENT;
  }

  Return<camera_common::Status> cancelBuffer(uint64_t id) override {
    ANativeWindowBuffer *buffer = findBuffer(id);
    return buffer ? mapWindowStatus(
                        window_->cancelBuffer(window_.get(), buffer, -1))
                  : camera_common::Status::ILLEGAL_ARGUMENT;
  }

  Return<camera_common::Status> setBufferCount(uint32_t count) override {
    native_window_api_disconnect(window_.get(), NATIVE_WINDOW_API_CAMERA);
    native_window_api_connect(window_.get(), NATIVE_WINDOW_API_CAMERA);
    if (width_ != 0) {
      native_window_set_buffers_dimensions(window_.get(), width_, height_);
      native_window_set_buffers_format(window_.get(), format_);
    }
    if (usage_ != 0)
      native_window_set_usage(window_.get(), usage_);
    if (swap_interval_ >= 0)
      window_->setSwapInterval(window_.get(), swap_interval_);
    const android::status_t status =
        native_window_set_buffer_count(window_.get(), count);
    if (status == android::OK) {
      std::lock_guard<std::mutex> lock(mutex_);
      ids_.clear();
      buffers_.clear();
    }
    return mapWindowStatus(status);
  }

  Return<camera_common::Status>
  setBuffersGeometry(uint32_t width, uint32_t height,
                     graphics_common::PixelFormat format) override {
    android::status_t status =
        native_window_set_buffers_dimensions(window_.get(), width, height);
    if (status == android::OK)
      status = native_window_set_buffers_format(window_.get(),
                                                static_cast<int>(format));
    if (status == android::OK) {
      width_ = width;
      height_ = height;
      format_ = static_cast<int>(format);
    }
    return mapWindowStatus(status);
  }

  Return<camera_common::Status>
  setCrop(int32_t left, int32_t top, int32_t right, int32_t bottom) override {
    const android_native_rect_t crop{left, top, right, bottom};
    return mapWindowStatus(native_window_set_crop(window_.get(), &crop));
  }

  Return<camera_common::Status>
  setUsage(graphics_common::BufferUsage usage) override {
    const android::status_t status =
        native_window_set_usage(window_.get(), static_cast<int>(usage));
    if (status == android::OK)
      usage_ = static_cast<int>(usage);
    return mapWindowStatus(status);
  }

  Return<camera_common::Status> setSwapInterval(int32_t interval) override {
    const android::status_t status =
        window_->setSwapInterval(window_.get(), interval);
    if (status == android::OK)
      swap_interval_ = interval;
    return mapWindowStatus(status);
  }

  Return<void> getMinUndequeuedBufferCount(
      getMinUndequeuedBufferCount_cb callback) override {
    int count = 0;
    const android::status_t status = window_->query(
        window_.get(), NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &count);
    callback(mapWindowStatus(status), count < 0 ? 0 : count);
    return Void();
  }

  Return<camera_common::Status> setTimestamp(int64_t timestamp) override {
    return mapWindowStatus(
        native_window_set_buffers_timestamp(window_.get(), timestamp));
  }

private:
  ANativeWindowBuffer *findBuffer(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = buffers_.find(id);
    return found == buffers_.end() ? nullptr : found->second;
  }

  sp<ANativeWindow> window_;
  std::mutex mutex_;
  std::map<buffer_handle_t, uint64_t, std::less<buffer_handle_t>> ids_;
  std::unordered_map<uint64_t, ANativeWindowBuffer *> buffers_;
  uint64_t next_id_ = 1;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  int format_ = 0;
  int usage_ = 0;
  int swap_interval_ = -1;
};

struct OpenDevice {
  sp<camera_device::ICameraDevice> device;
  sp<CameraCallback> callback;

  ~OpenDevice() {
    if (device) {
      device->stopPreview();
      device->close();
    }
  }
};

bool getParameters(const sp<camera_device::ICameraDevice> &device,
                   Parameters &parameters) {
  bool received = false;
  const auto result = device->getParameters([&](const hidl_string &value) {
    parameters = parseParameters(value.c_str());
    received = true;
  });
  return result.isOk() && received;
}

bool writeFile(const std::string &path, const std::vector<uint8_t> &bytes) {
  std::FILE *file = std::fopen(path.c_str(), "wb");
  if (!file)
    return false;
  const bool written =
      std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  const bool closed = std::fclose(file) == 0;
  return written && closed;
}

LensFacing parseFacing(camera_device::CameraFacing facing) {
  switch (facing) {
  case camera_device::CameraFacing::BACK:
    return LensFacing::Back;
  case camera_device::CameraFacing::FRONT:
    return LensFacing::Front;
  case camera_device::CameraFacing::EXTERNAL:
    return LensFacing::External;
  }
  return LensFacing::Unknown;
}

} // namespace

struct CameraManager::Implementation {
  sp<camera_provider::ICameraProvider> provider;
  std::vector<std::string> device_names;
  std::string error;
};

CameraManager::CameraManager()
    : implementation_(std::make_unique<Implementation>()) {}

CameraManager::~CameraManager() { shutdown(); }

bool CameraManager::initialize() {
  shutdown();
  static std::once_flag thread_pool_once;
  std::call_once(thread_pool_once,
                 [] { ::android::hardware::configureRpcThreadpool(2, false); });
  implementation_->provider =
      camera_provider::ICameraProvider::getService("legacy/0");
  if (!implementation_->provider) {
    implementation_->error = "Camera Provider HIDL service not found";
    return false;
  }
  camera_common::Status list_status = camera_common::Status::INTERNAL_ERROR;
  const auto result = implementation_->provider->getCameraIdList(
      [&](camera_common::Status status, const hidl_vec<hidl_string> &names) {
        list_status = status;
        for (const hidl_string &name : names) {
          const std::string value(name.c_str());
          if (isHal1Name(value))
            implementation_->device_names.push_back(value);
        }
      });
  if (!result.isOk() || list_status != camera_common::Status::OK) {
    implementation_->error = "Camera Provider enumeration failed";
    shutdown();
    return false;
  }
  implementation_->error.clear();
  return true;
}

void CameraManager::shutdown() {
  if (!implementation_)
    return;
  implementation_->device_names.clear();
  implementation_->provider.clear();
}

bool CameraManager::initialized() const {
  return implementation_ && implementation_->provider != nullptr;
}

template <typename Implementation>
static bool obtainDevice(Implementation *implementation,
                         const std::string &camera_id,
                         sp<camera_device::ICameraDevice> &device) {
  auto found = std::find_if(implementation->device_names.begin(),
                            implementation->device_names.end(),
                            [&](const std::string &name) {
                              return cameraIdFromName(name) == camera_id;
                            });
  if (found == implementation->device_names.end()) {
    implementation->error = "Camera Provider device ID was not found";
    return false;
  }
  camera_common::Status status = camera_common::Status::INTERNAL_ERROR;
  const auto result = implementation->provider->getCameraDeviceInterface_V1_x(
      *found, [&](camera_common::Status value,
                  const sp<camera_device::ICameraDevice> &interface) {
        status = value;
        device = interface;
      });
  if (!result.isOk() || status != camera_common::Status::OK || !device) {
    implementation->error = statusError("obtain camera device", status);
    return false;
  }
  return true;
}

bool CameraManager::enumerate(std::vector<CameraInfo> &cameras) {
  cameras.clear();
  if (!initialized()) {
    implementation_->error = "Camera manager is not initialized";
    return false;
  }
  for (const std::string &name : implementation_->device_names) {
    sp<camera_device::ICameraDevice> device;
    if (!obtainDevice(implementation_.get(), cameraIdFromName(name), device))
      return false;
    CameraInfo info;
    info.id = cameraIdFromName(name);
    camera_common::Status info_status = camera_common::Status::INTERNAL_ERROR;
    const auto info_result =
        device->getCameraInfo([&](camera_common::Status status,
                                  const camera_device::CameraInfo &value) {
          info_status = status;
          info.facing = parseFacing(value.facing);
          info.sensor_orientation = static_cast<int>(value.orientation);
        });
    if (!info_result.isOk() || info_status != camera_common::Status::OK) {
      implementation_->error =
          statusError("read camera information", info_status);
      return false;
    }
    info.hardware_level = 2;

    CaptureState state;
    sp<CameraCallback> callback = new CameraCallback(&state);
    camera_common::Status open_status = camera_common::Status::INTERNAL_ERROR;
    if (!resultStatus(device->open(callback), open_status) ||
        open_status != camera_common::Status::OK) {
      implementation_->error =
          statusError("open camera for parameters", open_status);
      return false;
    }
    Parameters parameters;
    const bool read = getParameters(device, parameters);
    device->close();
    if (!read) {
      implementation_->error = "read camera parameters failed";
      return false;
    }
    const auto jpeg_sizes = parseSizes(parameters["picture-size-values"]);
    const auto maximum = selectSize(jpeg_sizes, INT32_MAX, INT32_MAX);
    info.max_jpeg_width = maximum.first;
    info.max_jpeg_height = maximum.second;
    info.flash_available =
        valueListContains(parameters["flash-mode-values"], "on") ||
        valueListContains(parameters["flash-mode-values"], "torch");
    cameras.push_back(std::move(info));
  }
  implementation_->error.clear();
  return true;
}

bool CameraManager::setTorch(const std::string &camera_id, bool enabled) {
  if (!initialized() || camera_id.empty()) {
    implementation_->error = "Camera manager is not initialized or ID is empty";
    return false;
  }
  sp<camera_device::ICameraDevice> device;
  if (!obtainDevice(implementation_.get(), camera_id, device))
    return false;
  camera_common::Status status = camera_common::Status::INTERNAL_ERROR;
  if (!resultStatus(device->setTorchMode(enabled
                                             ? camera_common::TorchMode::ON
                                             : camera_common::TorchMode::OFF),
                    status) ||
      status != camera_common::Status::OK) {
    implementation_->error = statusError("set torch mode", status);
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
    implementation_->error = "invalid JPEG capture parameters";
    return false;
  }
  OpenDevice opened;
  if (!obtainDevice(implementation_.get(), camera_id, opened.device))
    return false;
  CaptureState state;
  opened.callback = new CameraCallback(&state);
  camera_common::Status status = camera_common::Status::INTERNAL_ERROR;
  if (!resultStatus(opened.device->open(opened.callback), status) ||
      status != camera_common::Status::OK) {
    implementation_->error = statusError("open camera", status);
    opened.device.clear();
    return false;
  }

  Parameters parameters;
  if (!getParameters(opened.device, parameters)) {
    implementation_->error = "read camera parameters failed";
    return false;
  }
  const auto photo_size = selectSize(
      parseSizes(parameters["picture-size-values"]), max_width, max_height);
  const auto preview_size =
      selectSize(parseSizes(parameters["preview-size-values"]), 640, 480);
  if (photo_size.first == 0 || preview_size.first == 0) {
    implementation_->error = "camera exposes no usable photo or preview size";
    return false;
  }
  parameters["picture-size"] = std::to_string(photo_size.first) + "x" +
                               std::to_string(photo_size.second);
  parameters["preview-size"] = std::to_string(preview_size.first) + "x" +
                               std::to_string(preview_size.second);
  parameters["picture-format"] = "jpeg";
  parameters["jpeg-quality"] = "90";
  parameters["flash-mode"] = flash ? "on" : "off";
  if (flash && !valueListContains(parameters["flash-mode-values"], "on")) {
    implementation_->error = "camera does not expose single-shot flash mode";
    return false;
  }
  const std::string serialized = serializeParameters(parameters);
  if (!resultStatus(opened.device->setParameters(serialized), status) ||
      status != camera_common::Status::OK) {
    implementation_->error = statusError("configure camera", status);
    return false;
  }

  sp<IGraphicBufferProducer> producer;
  sp<IGraphicBufferConsumer> consumer;
  BufferQueue::createBufferQueue(&producer, &consumer);
  sp<BufferItemConsumer> frame_consumer = new BufferItemConsumer(
      consumer, android::GraphicBuffer::USAGE_HW_TEXTURE);
  sp<FrameDrainer> drainer = new FrameDrainer(frame_consumer);
  frame_consumer->setFrameAvailableListener(drainer);
  sp<Surface> surface = new Surface(producer);
  sp<PreviewWindow> preview = new PreviewWindow(surface);
  if (!resultStatus(opened.device->setPreviewWindow(preview), status) ||
      status != camera_common::Status::OK) {
    implementation_->error = statusError("set camera preview window", status);
    return false;
  }
  if (!resultStatus(opened.device->startPreview(), status) ||
      status != camera_common::Status::OK) {
    implementation_->error = statusError("start camera preview", status);
    return false;
  }
  const int preview_timeout = std::min(timeout_ms, 5000);
  if (!drainer->waitForFrame(preview_timeout)) {
    implementation_->error = "camera preview produced no frame";
    return false;
  }

  opened.device->enableMsgType(
      static_cast<uint32_t>(camera_device::DataCallbackMsg::COMPRESSED_IMAGE));
  if (!resultStatus(opened.device->takePicture(), status) ||
      status != camera_common::Status::OK) {
    implementation_->error = statusError("take picture", status);
    return false;
  }
  {
    std::unique_lock<std::mutex> lock(state.mutex);
    if (!state.condition.wait_for(
            lock, std::chrono::milliseconds(timeout_ms),
            [&] { return !state.jpeg.empty() || state.callback_error; }) ||
        state.jpeg.empty()) {
      implementation_->error = state.callback_error
                                   ? "camera reported a capture callback error"
                                   : "JPEG capture timed out";
      return false;
    }
  }
  opened.device->disableMsgType(
      static_cast<uint32_t>(camera_device::DataCallbackMsg::COMPRESSED_IMAGE));
  if (!writeFile(path, state.jpeg)) {
    implementation_->error = "write JPEG file failed";
    return false;
  }
  result.path = path;
  result.width = photo_size.first;
  result.height = photo_size.second;
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
  return level == 2 ? "legacy" : "unknown";
}

} // namespace oos::hardware
