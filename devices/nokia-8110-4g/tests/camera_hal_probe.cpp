#include <hardware/camera.h>
#include <hardware/hardware.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {

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
  std::vector<unsigned char> jpeg;
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
    const auto *begin = static_cast<const unsigned char *>(owned->base) +
                        index * owned->buffer_size;
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

std::string setParameter(std::string parameters, const char *name,
                         const char *value) {
  const std::string prefix = std::string(name) + "=";
  size_t start = parameters.find(prefix);
  if (start == std::string::npos) {
    if (!parameters.empty() && parameters.back() != ';')
      parameters += ';';
    parameters += prefix + value;
    return parameters;
  }
  const size_t end = parameters.find(';', start);
  parameters.replace(start,
                     end == std::string::npos ? std::string::npos : end - start,
                     prefix + value);
  return parameters;
}

} // namespace

int main(int argc, char **argv) {
  const hw_module_t *hardware = nullptr;
  const int load = hw_get_module(CAMERA_HARDWARE_MODULE_ID, &hardware);
  if (load != 0 || !hardware) {
    std::fprintf(stderr, "camera HAL load failed: %d (%s)\n", load,
                 std::strerror(-load));
    return 1;
  }
  auto *module =
      reinterpret_cast<camera_module_t *>(const_cast<hw_module_t *>(hardware));
  std::printf("module_name=%s module_api=0x%04x hal_api=0x%04x "
              "torch_method=%d init_method=%d\n",
              module->common.name ? module->common.name : "",
              module->common.module_api_version, module->common.hal_api_version,
              module->set_torch_mode != nullptr, module->init != nullptr);
  if (module->init) {
    const int result = module->init();
    std::printf("module_init=%d\n", result);
    if (result != 0)
      return 1;
  }
  if (!module->get_number_of_cameras || !module->get_camera_info) {
    std::fprintf(stderr, "camera HAL enumeration methods are missing\n");
    return 1;
  }
  const int count = module->get_number_of_cameras();
  std::printf("camera_count=%d\n", count);
  if (count < 0)
    return 1;
  for (int id = 0; id < count; ++id) {
    camera_info info{};
    const int result = module->get_camera_info(id, &info);
    std::printf("camera.%d result=%d facing=%d orientation=%d "
                "device_api=0x%04x metadata=%d cost=%d conflicts=%zu\n",
                id, result, info.facing, info.orientation, info.device_version,
                info.static_camera_characteristics != nullptr,
                info.resource_cost, info.conflicting_devices_length);
  }
  if (argc > 1 && count > 0) {
    hw_device_t *hardware_device = nullptr;
    const int open_result =
        module->common.methods->open(&module->common, "0", &hardware_device);
    std::printf("camera_open=%d device=%d\n", open_result,
                hardware_device != nullptr);
    if (open_result != 0 || !hardware_device)
      return 1;
    auto *camera = reinterpret_cast<camera_device_t *>(hardware_device);
    char *parameters = camera->ops->get_parameters(camera);
    std::printf("parameters=%s\n", parameters ? parameters : "");
    std::string parameter_text = parameters ? parameters : "";
    if (parameters) {
      if (camera->ops->put_parameters)
        camera->ops->put_parameters(camera, parameters);
      else
        std::free(parameters);
    }
    if (!std::strcmp(argv[1], "capture")) {
      parameter_text =
          setParameter(std::move(parameter_text), "picture-size", "640x480");
      parameter_text =
          setParameter(std::move(parameter_text), "picture-format", "jpeg");
      parameter_text =
          setParameter(std::move(parameter_text), "flash-mode", "off");
      parameter_text =
          setParameter(std::move(parameter_text), "preview-size", "240x320");
      parameter_text =
          setParameter(std::move(parameter_text), "no-display-mode", "1");
      const int set_result =
          camera->ops->set_parameters(camera, parameter_text.c_str());
      std::printf("set_parameters=%d\n", set_result);
      CaptureState state;
      camera->ops->set_callbacks(camera, notifyCallback, dataCallback,
                                 timestampCallback, requestMemory, &state);
      camera->ops->enable_msg_type(camera, CAMERA_MSG_ERROR |
                                               CAMERA_MSG_SHUTTER |
                                               CAMERA_MSG_COMPRESSED_IMAGE);
      const int preview_result = camera->ops->start_preview(camera);
      std::printf("start_preview=%d\n", preview_result);
      if (preview_result == 0)
        usleep(500000);
      const int picture_result = preview_result == 0
                                     ? camera->ops->take_picture(camera)
                                     : preview_result;
      std::printf("take_picture=%d\n", picture_result);
      bool completed = false;
      if (picture_result == 0) {
        std::unique_lock<std::mutex> lock(state.mutex);
        completed =
            state.condition.wait_for(lock, std::chrono::seconds(15), [&] {
              return state.failed || !state.jpeg.empty();
            });
      }
      if (!completed || state.failed || state.jpeg.empty()) {
        std::fprintf(stderr, "capture failed completed=%d callback_error=%d\n",
                     completed, state.failed);
        camera->ops->cancel_picture(camera);
      } else {
        FILE *file = std::fopen("/data/local/tmp/oos-camera-test.jpg", "wb");
        const bool wrote =
            file && std::fwrite(state.jpeg.data(), 1, state.jpeg.size(),
                                file) == state.jpeg.size();
        if (file)
          std::fclose(file);
        std::printf("jpeg_bytes=%zu wrote=%d\n", state.jpeg.size(), wrote);
      }
      camera->ops->disable_msg_type(camera, CAMERA_MSG_ERROR |
                                                CAMERA_MSG_SHUTTER |
                                                CAMERA_MSG_COMPRESSED_IMAGE);
      camera->ops->stop_preview(camera);
    }
    if (camera->ops->release)
      camera->ops->release(camera);
    camera->common.close(&camera->common);
    std::printf("camera_close=ok\n");
  }
  return 0;
}
