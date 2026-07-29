#include "oos/runtime/wasm_app.h"

#include <wasm_export.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "oos/apps/zip_archive.h"
#include "oos/apps/permissions.h"
#include "oos/device/device.h"
#include "oos/device/service_provider.h"
#include "oos/runtime/graphics_host.h"
#include "oos/storage/app_storage.h"
#include "oos/storage/device_storage.h"

namespace oos::runtime {
namespace {

using WasmServicePermission = apps::DeviceServicePermission;

constexpr size_t kErrorBufferSize = 512;
constexpr uint32_t kMaxLogBytes = 4096;
constexpr size_t kMaxModuleBytes = 32 * 1024 * 1024;
constexpr const char *kRuntimeInterface = "oos:platform/runtime@0.1.0";
constexpr const char *kGraphicsInterface = "oos:platform/graphics@0.1.0";
constexpr const char *kGlesInterface = "oos:platform/gles@0.1.0";
constexpr const char *kDeviceInterface = "oos:platform/device@0.1.0";
constexpr const char *kAudioInterface = "oos:platform/audio@0.1.0";
constexpr const char *kCameraInterface = "oos:platform/camera@0.1.0";
constexpr const char *kPowerInterface = "oos:platform/power@0.1.0";
constexpr const char *kVibratorInterface = "oos:platform/vibrator@0.1.0";
constexpr const char *kWifiInterface = "oos:platform/wifi@0.1.0";
constexpr const char *kIpInterface = "oos:platform/ip@0.1.0";
constexpr const char *kBluetoothInterface = "oos:platform/bluetooth@0.1.0";
constexpr const char *kModemInterface = "oos:platform/modem@0.1.0";
constexpr const char *kCodecInterface = "oos:platform/codec@0.1.0";
constexpr const char *kStorageInterface = "oos:platform/storage@0.1.0";
constexpr const char *kDeviceStorageInterface =
    "oos:platform/device-storage@0.1.0";
constexpr const char *kLifecycleInit = "oos:platform/lifecycle@0.1.0#init";
constexpr const char *kLifecycleEvent = "oos:platform/lifecycle@0.1.0#event";
constexpr const char *kLifecycleFrame = "oos:platform/lifecycle@0.1.0#frame";
constexpr const char *kLifecycleShutdown =
    "oos:platform/lifecycle@0.1.0#shutdown";

enum class WitError : uint8_t {
  Unavailable = 0,
  InvalidArgument = 1,
  PermissionDenied = 2,
  LimitExceeded = 3,
  Io = 4,
  Timeout = 5,
  Busy = 6,
  Failed = 7,
};

const char *witErrorName(uint8_t error) {
  constexpr const char *kNames[] = {
      "unavailable",
      "invalid-argument",
      "permission-denied",
      "limit-exceeded",
      "io",
      "timeout",
      "busy",
      "failed",
  };
  return error < std::size(kNames) ? kNames[error] : "invalid-error-code";
}

template <typename T>
const T *appArray(wasm_exec_env_t environment, uint32_t offset, uint32_t count,
                  uint32_t maximum) {
  if (count > maximum)
    return nullptr;
  if (count == 0)
    return reinterpret_cast<const T *>(1);
  if (count > std::numeric_limits<uint32_t>::max() / sizeof(T))
    return nullptr;
  wasm_module_inst_t instance = wasm_runtime_get_module_inst(environment);
  const uint32_t bytes = count * sizeof(T);
  if (!wasm_runtime_validate_app_addr(instance, offset, bytes))
    return nullptr;
  return static_cast<const T *>(
      wasm_runtime_addr_app_to_native(instance, offset));
}

template <typename T>
T *appMutableArray(wasm_exec_env_t environment, uint32_t offset, uint32_t count,
                   uint32_t maximum) {
  return const_cast<T *>(appArray<T>(environment, offset, count, maximum));
}

struct AppHostContext {
  GraphicsHost *graphics = nullptr;
  device::Device *device = nullptr;
  std::unique_ptr<device::ServiceProvider> *services = nullptr;
  storage::AppStorage *storage = nullptr;
  storage::DeviceStorageService *device_storage = nullptr;
  uint32_t service_permission_mask = 0;
  bool enforce_service_permissions = false;
};

AppHostContext *hostFor(wasm_exec_env_t environment) {
  wasm_module_inst_t instance = wasm_runtime_get_module_inst(environment);
  return static_cast<AppHostContext *>(wasm_runtime_get_custom_data(instance));
}

GraphicsHost *graphicsFor(wasm_exec_env_t environment) {
  AppHostContext *host = hostFor(environment);
  return host ? host->graphics : nullptr;
}

storage::AppStorage *storageFor(wasm_exec_env_t environment) {
  AppHostContext *host = hostFor(environment);
  return host ? host->storage : nullptr;
}

constexpr uintptr_t kServiceErrorOffsetMask = 0xff;
constexpr unsigned kServicePermissionShift = 8;

void *serviceAttachment(size_t error_offset,
                        apps::DeviceServicePermission permission) {
  return reinterpret_cast<void *>(
      static_cast<uintptr_t>(error_offset) |
      (static_cast<uintptr_t>(apps::permissionBit(permission))
       << kServicePermissionShift));
}

uint32_t attachedServicePermission(wasm_exec_env_t environment) {
  return static_cast<uint32_t>(
      reinterpret_cast<uintptr_t>(
          wasm_runtime_get_function_attachment(environment)) >>
      kServicePermissionShift);
}

bool servicePermissionGranted(wasm_exec_env_t environment,
                              uint32_t required_permission) {
  const AppHostContext *host = hostFor(environment);
  return host &&
         (!host->enforce_service_permissions || required_permission == 0 ||
          (host->service_permission_mask & required_permission) != 0);
}

bool servicePermissionGranted(wasm_exec_env_t environment,
                              apps::DeviceServicePermission required_permission) {
  return servicePermissionGranted(environment,
                                  apps::permissionBit(required_permission));
}

WitError serviceAccessError(wasm_exec_env_t environment) {
  const uint32_t permission = attachedServicePermission(environment);
  return servicePermissionGranted(environment, permission)
             ? WitError::Unavailable
             : WitError::PermissionDenied;
}

WitError deviceStorageAccessError(wasm_exec_env_t environment) {
  return servicePermissionGranted(environment,
                                  apps::DeviceServicePermission::DeviceStorage)
             ? WitError::Unavailable
             : WitError::PermissionDenied;
}

storage::DeviceStorageService *deviceStorageFor(wasm_exec_env_t environment) {
  AppHostContext *host = hostFor(environment);
  return host && servicePermissionGranted(
                     environment, apps::DeviceServicePermission::DeviceStorage)
             ? host->device_storage
             : nullptr;
}

device::ServiceProvider *servicesFor(wasm_exec_env_t environment) {
  AppHostContext *host = hostFor(environment);
  if (!host || !host->device || !host->services ||
      !servicePermissionGranted(environment, attachedServicePermission(environment)))
    return nullptr;
  if (!*host->services)
    *host->services = std::make_unique<device::ServiceProvider>(*host->device);
  return host->services->get();
}

void trapInvalidReturnArea(wasm_exec_env_t environment) {
  wasm_runtime_set_exception(wasm_runtime_get_module_inst(environment),
                             "invalid WIT canonical return area");
}

bool writeResult(wasm_exec_env_t environment, uint32_t result_offset,
                 bool success, WitError error = WitError::Failed) {
  uint8_t *result = appMutableArray<uint8_t>(environment, result_offset, 2, 2);
  if (!result) {
    trapInvalidReturnArea(environment);
    return false;
  }
  result[0] = success ? 0 : 1;
  result[1] = static_cast<uint8_t>(error);
  return true;
}

uint32_t nativeAbiVersion(wasm_exec_env_t) { return OOS_WASM_ABI_VERSION; }

void nativeSurfaceSize(wasm_exec_env_t environment, uint32_t result_offset) {
  GraphicsHost *graphics = graphicsFor(environment);
  uint32_t *result =
      appMutableArray<uint32_t>(environment, result_offset, 2, 2);
  if (!result) {
    trapInvalidReturnArea(environment);
    return;
  }
  result[0] = graphics ? graphics->width() : 0;
  result[1] = graphics ? graphics->height() : 0;
}

uint32_t nativeSurfaceFormat(wasm_exec_env_t environment) {
  GraphicsHost *graphics = graphicsFor(environment);
  return graphics ? graphics->surfaceFormat() : OOS_TEXTURE_RGBA8888;
}

uint32_t nativeSupportedTextureFormats(wasm_exec_env_t environment) {
  GraphicsHost *graphics = graphicsFor(environment);
  return graphics ? graphics->supportedTextureFormats() : 0;
}

void nativeGraphicsLimits(wasm_exec_env_t environment, uint32_t result_offset) {
  uint32_t *result =
      appMutableArray<uint32_t>(environment, result_offset, 5, 5);
  if (!result) {
    trapInvalidReturnArea(environment);
    return;
  }
  result[0] = OOS_GFX_MAX_TEXTURE_SIZE;
  result[1] = OOS_GFX_MAX_TEXTURE_BYTES;
  result[2] = OOS_GFX_MAX_VERTICES;
  result[3] = OOS_GFX_MAX_INDICES;
  result[4] = OOS_GFX_MAX_DRAW_COMMANDS;
}

uint32_t nativeWallClockMinutes(wasm_exec_env_t) {
  const time_t now = std::time(nullptr);
  tm local = {};
  if (!localtime_r(&now, &local))
    return 0;
  return static_cast<uint32_t>(local.tm_hour * 60 + local.tm_min);
}

void nativeLog(wasm_exec_env_t environment, uint32_t level, uint32_t offset,
               uint32_t length) {
  if (length > kMaxLogBytes)
    return;
  const char *message =
      appArray<char>(environment, offset, length, kMaxLogBytes);
  if (!message)
    return;
  const char *label = level >= 3 ? "error" : level == 2 ? "warn" : "info";
  std::fprintf(stderr, "wasm[%s]: %.*s\n", label, static_cast<int>(length),
               length == 0 ? "" : message);
  std::fflush(stderr);
}

void nativeTextureSet(wasm_exec_env_t environment, uint32_t texture,
                      uint32_t format, uint32_t x, uint32_t y, uint32_t width,
                      uint32_t height, uint32_t row_stride, uint32_t flags,
                      uint32_t offset, uint32_t length,
                      uint32_t result_offset) {
  GraphicsHost *graphics = graphicsFor(environment);
  const uint32_t bytes_per_pixel = oosTextureBytesPerPixel(format);
  const bool row_overflow =
      bytes_per_pixel &&
      width > std::numeric_limits<uint32_t>::max() / bytes_per_pixel;
  const uint32_t row_bytes = row_overflow ? 0 : width * bytes_per_pixel;
  const uint64_t required_bytes =
      height == 0
          ? 0
          : static_cast<uint64_t>(row_stride) * (height - 1) + row_bytes;
  if (!graphics || texture == 0 || width == 0 || height == 0 ||
      bytes_per_pixel == 0 || row_overflow || row_stride < row_bytes ||
      width > OOS_GFX_MAX_TEXTURE_SIZE || height > OOS_GFX_MAX_TEXTURE_SIZE ||
      (flags & ~OOS_TEXTURE_FLAGS_MASK) != 0 || required_bytes != length ||
      length > OOS_GFX_MAX_TEXTURE_BYTES) {
    writeResult(environment, result_offset, false, WitError::InvalidArgument);
    return;
  }
  const uint8_t *rgba =
      appArray<uint8_t>(environment, offset, length, OOS_GFX_MAX_TEXTURE_BYTES);
  writeResult(environment, result_offset,
              rgba && graphics->setTexture(texture, format, x, y, width, height,
                                           row_stride, flags, rgba, length),
              rgba ? WitError::Failed : WitError::InvalidArgument);
}

void nativeTextureFree(wasm_exec_env_t environment, uint32_t texture,
                       uint32_t result_offset) {
  GraphicsHost *graphics = graphicsFor(environment);
  writeResult(environment, result_offset,
              graphics && graphics->freeTexture(texture));
}

void nativeSubmit(wasm_exec_env_t environment, uint32_t vertex_offset,
                  uint32_t vertex_count, uint32_t index_offset,
                  uint32_t index_count, uint32_t command_offset,
                  uint32_t command_count, uint32_t clear_rgba,
                  uint32_t result_offset) {
  GraphicsHost *graphics = graphicsFor(environment);
  const OosGfxVertex *vertices = appArray<OosGfxVertex>(
      environment, vertex_offset, vertex_count, OOS_GFX_MAX_VERTICES);
  const uint16_t *indices = appArray<uint16_t>(
      environment, index_offset, index_count, OOS_GFX_MAX_INDICES);
  const OosGfxDrawCommand *commands = appArray<OosGfxDrawCommand>(
      environment, command_offset, command_count, OOS_GFX_MAX_DRAW_COMMANDS);
  if (!graphics || !vertices || !indices || !commands) {
    writeResult(environment, result_offset, false, WitError::InvalidArgument);
    return;
  }
  writeResult(
      environment, result_offset,
      graphics->submit(vertex_count == 0 ? nullptr : vertices, vertex_count,
                       index_count == 0 ? nullptr : indices, index_count,
                       command_count == 0 ? nullptr : commands, command_count,
                       clear_rgba));
}

void nativeGlesCapabilities(wasm_exec_env_t environment,
                            uint32_t result_offset) {
  auto *result =
      appMutableArray<OosGlesCapabilities>(environment, result_offset, 1, 1);
  GraphicsHost *graphics = graphicsFor(environment);
  if (!result) {
    trapInvalidReturnArea(environment);
    return;
  }
  *result = {};
  if (graphics)
    graphics->glesCapabilities(*result);
}

void nativeGlesBufferSet(wasm_exec_env_t environment, uint32_t buffer,
                         uint32_t size, uint32_t usage, uint32_t data_offset,
                         uint32_t data_size, uint32_t result_offset) {
  GraphicsHost *graphics = graphicsFor(environment);
  const uint8_t *data = appArray<uint8_t>(environment, data_offset, data_size,
                                          OOS_GLES_MAX_BUFFER_BYTES);
  const bool valid = buffer != 0 && size != 0 &&
                     size <= OOS_GLES_MAX_BUFFER_BYTES && data &&
                     (data_size == 0 || data_size == size);
  writeResult(environment, result_offset,
              valid && graphics &&
                  graphics->setGlesBuffer(buffer, size, usage,
                                          data_size ? data : nullptr,
                                          data_size),
              valid ? WitError::Failed : WitError::InvalidArgument);
}

void nativeGlesBufferWrite(wasm_exec_env_t environment, uint32_t buffer,
                           uint32_t write_offset, uint32_t data_offset,
                           uint32_t data_size, uint32_t result_offset) {
  GraphicsHost *graphics = graphicsFor(environment);
  const uint8_t *data = appArray<uint8_t>(environment, data_offset, data_size,
                                          OOS_GLES_MAX_BUFFER_BYTES);
  const bool valid = buffer != 0 && data_size != 0 && data;
  writeResult(
      environment, result_offset,
      valid && graphics &&
          graphics->writeGlesBuffer(buffer, write_offset, data, data_size),
      valid ? WitError::Failed : WitError::InvalidArgument);
}

void nativeGlesBufferFree(wasm_exec_env_t environment, uint32_t buffer,
                          uint32_t result_offset) {
  GraphicsHost *graphics = graphicsFor(environment);
  writeResult(environment, result_offset,
              graphics && graphics->freeGlesBuffer(buffer));
}

void nativeGlesShaderSet(wasm_exec_env_t environment, uint32_t shader,
                         uint32_t stage, uint32_t source_offset,
                         uint32_t source_size, uint32_t result_offset) {
  GraphicsHost *graphics = graphicsFor(environment);
  const char *source = appArray<char>(environment, source_offset, source_size,
                                      OOS_GLES_MAX_SHADER_BYTES);
  const bool valid = shader != 0 && source_size != 0 && source;
  writeResult(environment, result_offset,
              valid && graphics &&
                  graphics->setGlesShader(shader, stage, source, source_size),
              valid ? WitError::Failed : WitError::InvalidArgument);
}

void nativeGlesShaderFree(wasm_exec_env_t environment, uint32_t shader,
                          uint32_t result_offset) {
  GraphicsHost *graphics = graphicsFor(environment);
  writeResult(environment, result_offset,
              graphics && graphics->freeGlesShader(shader));
}

void nativeGlesProgramSet(wasm_exec_env_t environment, uint32_t program,
                          uint32_t vertex_shader, uint32_t fragment_shader,
                          uint32_t result_offset) {
  GraphicsHost *graphics = graphicsFor(environment);
  writeResult(environment, result_offset,
              graphics && graphics->setGlesProgram(program, vertex_shader,
                                                   fragment_shader));
}

void nativeGlesProgramFree(wasm_exec_env_t environment, uint32_t program,
                           uint32_t result_offset) {
  GraphicsHost *graphics = graphicsFor(environment);
  writeResult(environment, result_offset,
              graphics && graphics->freeGlesProgram(program));
}

int32_t nativeGlesAttributeLocation(wasm_exec_env_t environment,
                                    uint32_t program, uint32_t name_offset,
                                    uint32_t name_size) {
  GraphicsHost *graphics = graphicsFor(environment);
  const char *name = appArray<char>(environment, name_offset, name_size, 255);
  return graphics && name
             ? graphics->glesAttributeLocation(program, name, name_size)
             : -1;
}

int32_t nativeGlesUniformLocation(wasm_exec_env_t environment, uint32_t program,
                                  uint32_t name_offset, uint32_t name_size) {
  GraphicsHost *graphics = graphicsFor(environment);
  const char *name = appArray<char>(environment, name_offset, name_size, 255);
  return graphics && name
             ? graphics->glesUniformLocation(program, name, name_size)
             : -1;
}

void nativeGlesSubmit(wasm_exec_env_t environment, uint32_t command_offset,
                      uint32_t command_count, uint32_t data_offset,
                      uint32_t data_words, uint32_t result_offset) {
  GraphicsHost *graphics = graphicsFor(environment);
  const OosGlesCommand *commands = appArray<OosGlesCommand>(
      environment, command_offset, command_count, OOS_GLES_MAX_COMMANDS);
  const uint32_t *data = appArray<uint32_t>(
      environment, data_offset, data_words, OOS_GLES_MAX_COMMAND_DATA_WORDS);
  const bool valid = command_count >= 2 && commands && data;
  writeResult(environment, result_offset,
              valid && graphics &&
                  graphics->submitGles(commands, command_count,
                                       data_words ? data : nullptr, data_words),
              valid ? WitError::Failed : WitError::InvalidArgument);
}

uint32_t guestRealloc(wasm_exec_env_t environment, uint32_t old_pointer,
                      uint32_t old_size, uint32_t alignment,
                      uint32_t new_size) {
  wasm_module_inst_t instance = wasm_runtime_get_module_inst(environment);
  wasm_function_inst_t realloc =
      wasm_runtime_lookup_function(instance, "cabi_realloc");
  if (!realloc) {
    wasm_runtime_set_exception(instance,
                               "WIT guest does not export cabi_realloc");
    return 0;
  }
  uint32_t arguments[4] = {old_pointer, old_size, alignment, new_size};
  if (!wasm_runtime_call_wasm(environment, realloc, std::size(arguments),
                              arguments)) {
    return 0;
  }
  return arguments[0];
}

bool lowerString(wasm_exec_env_t environment, const char *value,
                 uint32_t &pointer, uint32_t &length) {
  value = value ? value : "";
  const size_t native_length = std::strlen(value);
  if (native_length > kMaxLogBytes)
    return false;
  length = static_cast<uint32_t>(native_length);
  if (length == 0) {
    pointer = 1;
    return true;
  }
  pointer = guestRealloc(environment, 0, 0, 1, length);
  uint8_t *destination =
      appMutableArray<uint8_t>(environment, pointer, length, kMaxLogBytes);
  if (!pointer || !destination)
    return false;
  std::memcpy(destination, value, length);
  return true;
}

void nativeDeviceDescriptor(wasm_exec_env_t environment,
                            uint32_t result_offset) {
  AppHostContext *host = hostFor(environment);
  uint32_t *result =
      appMutableArray<uint32_t>(environment, result_offset, 11, 11);
  if (!result) {
    trapInvalidReturnArea(environment);
    return;
  }
  const device::DeviceDescriptor empty_descriptor = {};
  const device::DeviceDescriptor &descriptor =
      host && host->device ? host->device->descriptor() : empty_descriptor;
  if (!lowerString(environment, descriptor.id, result[0], result[1]) ||
      !lowerString(environment, descriptor.manufacturer, result[2],
                   result[3]) ||
      !lowerString(environment, descriptor.model, result[4], result[5])) {
    wasm_runtime_set_exception(wasm_runtime_get_module_inst(environment),
                               "failed to lower WIT device descriptor");
    return;
  }
  result[6] = descriptor.android_api;
  result[7] = descriptor.primary_width;
  result[8] = descriptor.primary_height;
  result[9] = descriptor.secondary_width;
  result[10] = descriptor.secondary_height;
}

uint32_t nativeDeviceCapability(wasm_exec_env_t environment, uint32_t feature) {
  AppHostContext *host = hostFor(environment);
  if (!host || !host->device ||
      feature >= static_cast<uint32_t>(device::Feature::Count)) {
    return static_cast<uint32_t>(device::CapabilityState::Unsupported);
  }
  return static_cast<uint32_t>(
      host->device->capability(static_cast<device::Feature>(feature)));
}

void writeUnavailable(wasm_exec_env_t environment, uint32_t result_offset) {
  const size_t error_offset = reinterpret_cast<uintptr_t>(
                                  wasm_runtime_get_function_attachment(
                                      environment)) &
                              kServiceErrorOffsetMask;
  if (error_offset > 64) {
    trapInvalidReturnArea(environment);
    return;
  }
  uint8_t *result = appMutableArray<uint8_t>(
      environment, result_offset, static_cast<uint32_t>(error_offset + 1), 65);
  if (!result) {
    trapInvalidReturnArea(environment);
    return;
  }
  result[0] = 1;
  result[error_offset] = static_cast<uint8_t>(serviceAccessError(environment));
}

template <typename T>
void storeCanonical(uint8_t *area, size_t offset, T value) {
  std::memcpy(area + offset, &value, sizeof(value));
}

uint8_t *serviceResultArea(wasm_exec_env_t environment, uint32_t result_offset,
                           uint32_t size) {
  if (!servicesFor(environment)) {
    writeUnavailable(environment, result_offset);
    return nullptr;
  }
  uint8_t *result =
      appMutableArray<uint8_t>(environment, result_offset, size, size);
  if (!result) {
    trapInvalidReturnArea(environment);
    return nullptr;
  }
  std::memset(result, 0, size);
  return result;
}

uint8_t *allocateGuestRecord(wasm_exec_env_t environment, uint32_t size,
                             uint32_t alignment, uint32_t &pointer) {
  pointer = guestRealloc(environment, 0, 0, alignment, size);
  uint8_t *record =
      appMutableArray<uint8_t>(environment, pointer, size, 16 * 1024);
  if (!pointer || !record) {
    wasm_runtime_set_exception(wasm_runtime_get_module_inst(environment),
                               "failed to allocate WIT service result");
    return nullptr;
  }
  std::memset(record, 0, size);
  return record;
}

void failServiceResult(uint8_t *result, size_t error_offset,
                       WitError error = WitError::Io) {
  result[0] = 1;
  result[error_offset] = static_cast<uint8_t>(error);
}

void lowerBattery(const hardware::BatterySnapshot &snapshot, uint8_t *record) {
  record[0] = static_cast<uint8_t>(snapshot.state);
  storeCanonical<int32_t>(record, 4, snapshot.capacity_percent);
  storeCanonical<int32_t>(record, 8, snapshot.voltage_microvolts);
  storeCanonical<int32_t>(record, 12, snapshot.current_microamps);
  storeCanonical<int32_t>(record, 16, snapshot.temperature_tenths_celsius);
  record[20] = snapshot.usb_online;
}

bool lowerStringAt(wasm_exec_env_t environment, const char *value,
                   uint8_t *record, size_t offset) {
  uint32_t pointer = 0;
  uint32_t length = 0;
  if (!lowerString(environment, value, pointer, length))
    return false;
  storeCanonical(record, offset, pointer);
  storeCanonical(record, offset + 4, length);
  return true;
}

bool guestString(wasm_exec_env_t environment, uint32_t offset, uint32_t length,
                 std::string &value, uint32_t maximum = 256) {
  const char *bytes = appArray<char>(environment, offset, length, maximum);
  if (!bytes)
    return false;
  value.assign(length ? bytes : "", length);
  return true;
}

constexpr uint32_t kMaxDeviceStorageEntries = 8192;
constexpr uint32_t kDeviceStorageEntrySize = 24;
constexpr uint32_t kMaxDeviceStorageBytes = 64 * 1024 * 1024;

bool deviceStorageArguments(wasm_exec_env_t environment, uint32_t volume,
                            uint32_t path_offset, uint32_t path_length,
                            std::string &path) {
  return volume <=
             static_cast<uint32_t>(storage::DeviceStorageVolume::Removable) &&
         guestString(environment, path_offset, path_length, path, 4096) &&
         apps::validPackagePath(path) && path.back() != '/';
}

void nativeDeviceStorageEnumerate(wasm_exec_env_t environment, uint32_t volume,
                                  uint32_t result_offset) {
  uint8_t *result =
      appMutableArray<uint8_t>(environment, result_offset, 12, 12);
  if (!result) {
    trapInvalidReturnArea(environment);
    return;
  }
  std::memset(result, 0, 12);
  storage::DeviceStorageService *service = deviceStorageFor(environment);
  if (!service) {
    result[0] = 1;
    result[4] = static_cast<uint8_t>(deviceStorageAccessError(environment));
    return;
  }
  if (volume > static_cast<uint32_t>(storage::DeviceStorageVolume::Removable)) {
    result[0] = 1;
    result[4] = static_cast<uint8_t>(WitError::InvalidArgument);
    return;
  }
  std::vector<storage::DeviceStorageEntry> entries;
  if (!service->list(static_cast<storage::DeviceStorageVolume>(volume),
                     entries) ||
      entries.size() > kMaxDeviceStorageEntries) {
    result[0] = 1;
    result[4] = static_cast<uint8_t>(WitError::Io);
    return;
  }
  const uint32_t count = static_cast<uint32_t>(entries.size());
  const uint32_t bytes = count * kDeviceStorageEntrySize;
  const uint32_t pointer =
      count ? guestRealloc(environment, 0, 0, 8, bytes) : 8;
  uint8_t *records = count
                         ? appMutableArray<uint8_t>(environment, pointer, bytes,
                                                    kMaxDeviceStorageEntries *
                                                        kDeviceStorageEntrySize)
                         : reinterpret_cast<uint8_t *>(1);
  if (!pointer || !records) {
    wasm_runtime_set_exception(wasm_runtime_get_module_inst(environment),
                               "failed to lower device storage entries");
    return;
  }
  if (count)
    std::memset(records, 0, bytes);
  for (uint32_t index = 0; index < count; ++index) {
    uint32_t path_pointer = 0;
    uint32_t path_length = 0;
    if (!lowerString(environment, entries[index].path.c_str(), path_pointer,
                     path_length)) {
      wasm_runtime_set_exception(wasm_runtime_get_module_inst(environment),
                                 "failed to lower device storage path");
      return;
    }
    uint8_t *record = records + index * kDeviceStorageEntrySize;
    storeCanonical(record, 0, path_pointer);
    storeCanonical(record, 4, path_length);
    storeCanonical(record, 8, entries[index].size);
    storeCanonical(record, 16, entries[index].last_modified_ms);
  }
  result[0] = 0;
  storeCanonical(result, 4, pointer);
  storeCanonical(result, 8, count);
}

void nativeDeviceStorageRead(wasm_exec_env_t environment, uint32_t volume,
                             uint32_t path_offset, uint32_t path_length,
                             uint32_t result_offset) {
  uint8_t *result =
      appMutableArray<uint8_t>(environment, result_offset, 12, 12);
  if (!result) {
    trapInvalidReturnArea(environment);
    return;
  }
  std::memset(result, 0, 12);
  storage::DeviceStorageService *service = deviceStorageFor(environment);
  std::string path;
  const bool arguments = deviceStorageArguments(environment, volume,
                                                path_offset, path_length, path);
  if (!service || !arguments) {
    result[0] = 1;
    result[4] = static_cast<uint8_t>(service ? WitError::InvalidArgument
                                             : deviceStorageAccessError(
                                                   environment));
    return;
  }
  uint64_t native_size = 0;
  if (!service->fileSize(static_cast<storage::DeviceStorageVolume>(volume),
                         path, native_size) ||
      native_size > kMaxDeviceStorageBytes) {
    result[0] = 1;
    result[4] = static_cast<uint8_t>(WitError::Io);
    return;
  }
  const uint32_t size = static_cast<uint32_t>(native_size);
  const uint32_t pointer = size ? guestRealloc(environment, 0, 0, 1, size) : 1;
  uint8_t *destination =
      size ? appMutableArray<uint8_t>(environment, pointer, size,
                                      kMaxDeviceStorageBytes)
           : reinterpret_cast<uint8_t *>(1);
  if (!pointer || !destination) {
    wasm_runtime_set_exception(wasm_runtime_get_module_inst(environment),
                               "failed to allocate device storage result");
    return;
  }
  size_t bytes_read = 0;
  if (!service->readInto(static_cast<storage::DeviceStorageVolume>(volume),
                         path, size ? destination : nullptr, size,
                         bytes_read) ||
      bytes_read != size) {
    if (size)
      guestRealloc(environment, pointer, size, 1, 0);
    result[0] = 1;
    result[4] = static_cast<uint8_t>(WitError::Io);
    return;
  }
  result[0] = 0;
  storeCanonical(result, 4, pointer);
  storeCanonical(result, 8, size);
}

void nativeDeviceStorageWrite(wasm_exec_env_t environment, uint32_t volume,
                              uint32_t path_offset, uint32_t path_length,
                              uint32_t mode, uint32_t bytes_offset,
                              uint32_t bytes_length, uint32_t result_offset) {
  storage::DeviceStorageService *service = deviceStorageFor(environment);
  std::string path;
  const bool path_valid = deviceStorageArguments(
      environment, volume, path_offset, path_length, path);
  const uint8_t *bytes = appArray<uint8_t>(
      environment, bytes_offset, bytes_length, kMaxDeviceStorageBytes);
  const bool arguments =
      path_valid && bytes &&
      mode <= static_cast<uint32_t>(storage::DeviceStorageWriteMode::Append);
  writeResult(environment, result_offset,
              service && arguments &&
                  service->write(
                      static_cast<storage::DeviceStorageVolume>(volume), path,
                      static_cast<storage::DeviceStorageWriteMode>(mode),
                      bytes_length ? bytes : nullptr, bytes_length),
              service ? (arguments ? WitError::Io : WitError::InvalidArgument)
                      : deviceStorageAccessError(environment));
}

void nativeDeviceStorageDelete(wasm_exec_env_t environment, uint32_t volume,
                               uint32_t path_offset, uint32_t path_length,
                               uint32_t result_offset) {
  uint8_t *result = appMutableArray<uint8_t>(environment, result_offset, 2, 2);
  if (!result) {
    trapInvalidReturnArea(environment);
    return;
  }
  storage::DeviceStorageService *service = deviceStorageFor(environment);
  std::string path;
  const bool arguments = deviceStorageArguments(environment, volume,
                                                path_offset, path_length, path);
  bool removed = false;
  const bool success =
      service && arguments &&
      service->remove(static_cast<storage::DeviceStorageVolume>(volume), path,
                      removed);
  result[0] = success ? 0 : 1;
  result[1] =
      success
          ? static_cast<uint8_t>(removed)
          : static_cast<uint8_t>(
                service ? (arguments ? WitError::Io : WitError::InvalidArgument)
                        : deviceStorageAccessError(environment));
}

void nativeDeviceStorageSpace(wasm_exec_env_t environment, uint32_t volume,
                              uint32_t result_offset) {
  uint8_t *result =
      appMutableArray<uint8_t>(environment, result_offset, 16, 16);
  if (!result) {
    trapInvalidReturnArea(environment);
    return;
  }
  std::memset(result, 0, 16);
  const bool free = reinterpret_cast<uintptr_t>(
                        wasm_runtime_get_function_attachment(environment)) == 0;
  storage::DeviceStorageService *service = deviceStorageFor(environment);
  uint64_t bytes = 0;
  const bool arguments =
      volume <= static_cast<uint32_t>(storage::DeviceStorageVolume::Removable);
  const bool success =
      service && arguments &&
      (free ? service->freeSpace(
                  static_cast<storage::DeviceStorageVolume>(volume), bytes)
            : service->usedSpace(
                  static_cast<storage::DeviceStorageVolume>(volume), bytes));
  result[0] = success ? 0 : 1;
  if (success) {
    storeCanonical(result, 8, bytes);
  } else {
    result[8] = static_cast<uint8_t>(
        service ? (arguments ? WitError::Io : WitError::InvalidArgument)
                : deviceStorageAccessError(environment));
  }
}

void nativeKvGet(wasm_exec_env_t environment, uint32_t key_offset,
                 uint32_t key_length, uint32_t result_offset) {
  uint8_t *result =
      appMutableArray<uint8_t>(environment, result_offset, 16, 16);
  if (!result) {
    trapInvalidReturnArea(environment);
    return;
  }
  std::memset(result, 0, 16);
  std::string key;
  storage::AppStorage *app_storage = storageFor(environment);
  if (!app_storage) {
    result[0] = 1;
    result[4] = static_cast<uint8_t>(WitError::Unavailable);
    return;
  }
  if (!guestString(environment, key_offset, key_length, key)) {
    result[0] = 1;
    result[4] = static_cast<uint8_t>(WitError::InvalidArgument);
    return;
  }
  std::vector<uint8_t> value;
  bool found = false;
  if (!app_storage->get(key, value, found)) {
    result[0] = 1;
    result[4] = static_cast<uint8_t>(WitError::Io);
    return;
  }
  result[0] = 0;
  result[4] = found ? 1 : 0;
  if (!found)
    return;
  const uint32_t pointer =
      value.empty() ? 1
                    : guestRealloc(environment, 0, 0, 1,
                                   static_cast<uint32_t>(value.size()));
  uint8_t *destination =
      value.empty()
          ? reinterpret_cast<uint8_t *>(1)
          : appMutableArray<uint8_t>(environment, pointer,
                                     static_cast<uint32_t>(value.size()),
                                     4 * 1024 * 1024);
  if (!pointer || !destination) {
    wasm_runtime_set_exception(wasm_runtime_get_module_inst(environment),
                               "failed to lower KV value");
    return;
  }
  if (!value.empty())
    std::memcpy(destination, value.data(), value.size());
  storeCanonical(result, 8, pointer);
  storeCanonical(result, 12, static_cast<uint32_t>(value.size()));
}

void nativeKvSet(wasm_exec_env_t environment, uint32_t key_offset,
                 uint32_t key_length, uint32_t value_offset,
                 uint32_t value_length, uint32_t result_offset) {
  std::string key;
  const uint8_t *value = appArray<uint8_t>(environment, value_offset,
                                           value_length, 4 * 1024 * 1024);
  storage::AppStorage *app_storage = storageFor(environment);
  if (!app_storage) {
    writeResult(environment, result_offset, false, WitError::Unavailable);
    return;
  }
  if (!value || !guestString(environment, key_offset, key_length, key)) {
    writeResult(environment, result_offset, false, WitError::InvalidArgument);
    return;
  }
  writeResult(
      environment, result_offset,
      app_storage->set(key, value_length ? value : nullptr, value_length),
      WitError::Io);
}

void nativeKvDelete(wasm_exec_env_t environment, uint32_t key_offset,
                    uint32_t key_length, uint32_t result_offset) {
  std::string key;
  storage::AppStorage *app_storage = storageFor(environment);
  if (!app_storage) {
    writeResult(environment, result_offset, false, WitError::Unavailable);
    return;
  }
  bool removed = false;
  writeResult(environment, result_offset,
              guestString(environment, key_offset, key_length, key) &&
                  app_storage->remove(key, removed),
              WitError::Io);
}

void nativeKvClear(wasm_exec_env_t environment, uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  writeResult(environment, result_offset, app_storage && app_storage->clear(),
              app_storage ? WitError::Io : WitError::Unavailable);
}

void writeU32Result(wasm_exec_env_t environment, uint32_t result_offset,
                    bool success, uint32_t value,
                    WitError error = WitError::Io) {
  uint8_t *result = appMutableArray<uint8_t>(environment, result_offset, 8, 8);
  if (!result) {
    trapInvalidReturnArea(environment);
    return;
  }
  std::memset(result, 0, 8);
  result[0] = success ? 0 : 1;
  storeCanonical(result, 4, success ? value : static_cast<uint32_t>(error));
}

void writeEnumResult(wasm_exec_env_t environment, uint32_t result_offset,
                     bool success, uint8_t value,
                     WitError error = WitError::Io) {
  uint8_t *result = appMutableArray<uint8_t>(environment, result_offset, 2, 2);
  if (!result) {
    trapInvalidReturnArea(environment);
    return;
  }
  result[0] = success ? 0 : 1;
  result[1] = success ? value : static_cast<uint8_t>(error);
}

template <typename T>
void writeWideResult(wasm_exec_env_t environment, uint32_t result_offset,
                     bool success, T value, WitError error = WitError::Io) {
  static_assert(sizeof(T) == 8);
  uint8_t *result =
      appMutableArray<uint8_t>(environment, result_offset, 16, 16);
  if (!result) {
    trapInvalidReturnArea(environment);
    return;
  }
  std::memset(result, 0, 16);
  result[0] = success ? 0 : 1;
  if (success)
    storeCanonical(result, 8, value);
  else
    result[8] = static_cast<uint8_t>(error);
}

bool writeBytesResult(wasm_exec_env_t environment, uint32_t result_offset,
                      const uint8_t *bytes, uint32_t size, bool success,
                      WitError error = WitError::Io) {
  uint8_t *result =
      appMutableArray<uint8_t>(environment, result_offset, 12, 12);
  if (!result) {
    trapInvalidReturnArea(environment);
    return false;
  }
  std::memset(result, 0, 12);
  result[0] = success ? 0 : 1;
  if (!success) {
    result[4] = static_cast<uint8_t>(error);
    return true;
  }
  const uint32_t pointer =
      size == 0 ? 1 : guestRealloc(environment, 0, 0, 1, size);
  uint8_t *destination = size == 0
                             ? reinterpret_cast<uint8_t *>(1)
                             : appMutableArray<uint8_t>(environment, pointer,
                                                        size, 4 * 1024 * 1024);
  if (!pointer || !destination) {
    wasm_runtime_set_exception(wasm_runtime_get_module_inst(environment),
                               "failed to lower SQLite value");
    return false;
  }
  if (size)
    std::memcpy(destination, bytes, size);
  storeCanonical(result, 4, pointer);
  storeCanonical(result, 8, size);
  return true;
}

bool databaseArguments(wasm_exec_env_t environment, uint32_t database_offset,
                       uint32_t database_length, uint32_t sql_offset,
                       uint32_t sql_length, std::string &database,
                       std::string &sql) {
  return guestString(environment, database_offset, database_length, database,
                     64) &&
         guestString(environment, sql_offset, sql_length, sql, 64 * 1024);
}

void nativeDatabaseExecute(wasm_exec_env_t environment,
                           uint32_t database_offset, uint32_t database_length,
                           uint32_t sql_offset, uint32_t sql_length,
                           uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  std::string database;
  std::string sql;
  uint32_t changes = 0;
  const bool arguments =
      databaseArguments(environment, database_offset, database_length,
                        sql_offset, sql_length, database, sql);
  const bool success = app_storage && arguments &&
                       app_storage->databaseExecute(database, sql, changes);
  writeU32Result(environment, result_offset, success, changes,
                 app_storage
                     ? (arguments ? WitError::Io : WitError::InvalidArgument)
                     : WitError::Unavailable);
}

void nativeDatabasePrepare(wasm_exec_env_t environment,
                           uint32_t database_offset, uint32_t database_length,
                           uint32_t sql_offset, uint32_t sql_length,
                           uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  std::string database;
  std::string sql;
  uint32_t statement = 0;
  const bool arguments =
      databaseArguments(environment, database_offset, database_length,
                        sql_offset, sql_length, database, sql);
  const bool success = app_storage && arguments &&
                       app_storage->databasePrepare(database, sql, statement);
  writeU32Result(environment, result_offset, success, statement,
                 app_storage
                     ? (arguments ? WitError::Io : WitError::InvalidArgument)
                     : WitError::Unavailable);
}

void nativeStatementStep(wasm_exec_env_t environment, uint32_t statement,
                         uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  storage::SqlRowState state = storage::SqlRowState::Done;
  const bool success =
      app_storage && app_storage->statementStep(statement, state);
  writeEnumResult(environment, result_offset, success,
                  static_cast<uint8_t>(state),
                  app_storage ? WitError::Io : WitError::Unavailable);
}

void nativeStatementBindNull(wasm_exec_env_t environment, uint32_t statement,
                             uint32_t index, uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  writeResult(environment, result_offset,
              app_storage && app_storage->statementBindNull(statement, index),
              app_storage ? WitError::Io : WitError::Unavailable);
}

void nativeStatementBindInteger(wasm_exec_env_t environment, uint32_t statement,
                                uint32_t index, int64_t value,
                                uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  writeResult(environment, result_offset,
              app_storage &&
                  app_storage->statementBindInteger(statement, index, value),
              app_storage ? WitError::Io : WitError::Unavailable);
}

void nativeStatementBindFloat(wasm_exec_env_t environment, uint32_t statement,
                              uint32_t index, double value,
                              uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  writeResult(environment, result_offset,
              app_storage &&
                  app_storage->statementBindFloat(statement, index, value),
              app_storage ? WitError::Io : WitError::Unavailable);
}

void nativeStatementBindText(wasm_exec_env_t environment, uint32_t statement,
                             uint32_t index, uint32_t value_offset,
                             uint32_t value_length, uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  std::string value;
  const bool arguments = guestString(environment, value_offset, value_length,
                                     value, 4 * 1024 * 1024);
  writeResult(environment, result_offset,
              app_storage && arguments &&
                  app_storage->statementBindText(statement, index, value),
              app_storage
                  ? (arguments ? WitError::Io : WitError::InvalidArgument)
                  : WitError::Unavailable);
}

void nativeStatementBindBlob(wasm_exec_env_t environment, uint32_t statement,
                             uint32_t index, uint32_t value_offset,
                             uint32_t value_length, uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  const uint8_t *value = appArray<uint8_t>(environment, value_offset,
                                           value_length, 4 * 1024 * 1024);
  writeResult(environment, result_offset,
              app_storage && value &&
                  app_storage->statementBindBlob(statement, index,
                                                 value_length ? value : nullptr,
                                                 value_length),
              app_storage ? (value ? WitError::Io : WitError::InvalidArgument)
                          : WitError::Unavailable);
}

void nativeStatementColumnCount(wasm_exec_env_t environment, uint32_t statement,
                                uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  uint32_t count = 0;
  const bool success =
      app_storage && app_storage->statementColumnCount(statement, count);
  writeU32Result(environment, result_offset, success, count,
                 app_storage ? WitError::Io : WitError::Unavailable);
}

void nativeStatementColumnKind(wasm_exec_env_t environment, uint32_t statement,
                               uint32_t column, uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  storage::SqlValueKind kind = storage::SqlValueKind::Null;
  const bool success =
      app_storage && app_storage->statementColumnKind(statement, column, kind);
  writeEnumResult(environment, result_offset, success,
                  static_cast<uint8_t>(kind),
                  app_storage ? WitError::Io : WitError::Unavailable);
}

void nativeStatementColumnInteger(wasm_exec_env_t environment,
                                  uint32_t statement, uint32_t column,
                                  uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  int64_t value = 0;
  const bool success = app_storage && app_storage->statementColumnInt64(
                                          statement, column, value);
  writeWideResult(environment, result_offset, success, value,
                  app_storage ? WitError::Io : WitError::Unavailable);
}

void nativeStatementColumnFloat(wasm_exec_env_t environment, uint32_t statement,
                                uint32_t column, uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  double value = 0;
  const bool success = app_storage && app_storage->statementColumnDouble(
                                          statement, column, value);
  writeWideResult(environment, result_offset, success, value,
                  app_storage ? WitError::Io : WitError::Unavailable);
}

void nativeStatementColumnText(wasm_exec_env_t environment, uint32_t statement,
                               uint32_t column, uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  std::string value;
  const bool success =
      app_storage && app_storage->statementColumnText(statement, column, value);
  writeBytesResult(environment, result_offset,
                   reinterpret_cast<const uint8_t *>(value.data()),
                   static_cast<uint32_t>(value.size()), success,
                   app_storage ? WitError::Io : WitError::Unavailable);
}

void nativeStatementColumnBlob(wasm_exec_env_t environment, uint32_t statement,
                               uint32_t column, uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  std::vector<uint8_t> value;
  const bool success =
      app_storage && app_storage->statementColumnBlob(statement, column, value);
  writeBytesResult(environment, result_offset, value.data(),
                   static_cast<uint32_t>(value.size()), success,
                   app_storage ? WitError::Io : WitError::Unavailable);
}

void nativeStatementFinish(wasm_exec_env_t environment, uint32_t statement,
                           uint32_t result_offset) {
  storage::AppStorage *app_storage = storageFor(environment);
  writeResult(environment, result_offset,
              app_storage && app_storage->statementFinish(statement),
              app_storage ? WitError::Io : WitError::Unavailable);
}

void nativeServiceMessage(wasm_exec_env_t environment, uint32_t result_offset) {
  uint32_t *result =
      appMutableArray<uint32_t>(environment, result_offset, 2, 2);
  const bool permission_granted = servicePermissionGranted(
      environment, attachedServicePermission(environment));
  device::ServiceProvider *services =
      permission_granted ? servicesFor(environment) : nullptr;
  const std::string message =
      !permission_granted     ? "permission denied"
      : !services             ? "service unavailable"
      : services->lastError().empty() ? "service ready"
                                      : services->lastError();
  if (!result ||
      !lowerString(environment, message.c_str(), result[0], result[1])) {
    trapInvalidReturnArea(environment);
  }
}

void nativeAudioPlayTone(wasm_exec_env_t environment, double frequency_hz,
                         uint32_t duration_ms, float volume, uint32_t usage,
                         uint32_t result_offset) {
  uint8_t *result = serviceResultArea(environment, result_offset, 32);
  if (!result)
    return;
  device::ServiceProvider *services = servicesFor(environment);
  hardware::AudioStreamInfo info;
  if (usage > static_cast<uint32_t>(hardware::AudioUsage::Notification)) {
    failServiceResult(result, 8, WitError::InvalidArgument);
    return;
  }
  if (!services->playTone(frequency_hz, static_cast<int>(duration_ms), volume,
                          static_cast<hardware::AudioUsage>(usage), info)) {
    failServiceResult(result, 8);
    return;
  }
  storeCanonical<int32_t>(result, 8, info.sample_rate);
  storeCanonical<int32_t>(result, 12, info.channel_count);
  storeCanonical<int32_t>(result, 16, info.device_id);
  storeCanonical<int64_t>(result, 24, info.frames_transferred);
}

void nativeAudioRecord(wasm_exec_env_t environment, uint32_t path_offset,
                       uint32_t path_length, uint32_t duration_ms,
                       uint32_t result_offset) {
  uint8_t *result = serviceResultArea(environment, result_offset, 56);
  if (!result)
    return;
  std::string path;
  if (!guestString(environment, path_offset, path_length, path, 4096)) {
    failServiceResult(result, 8, WitError::InvalidArgument);
    return;
  }
  hardware::RecordingResult recording;
  if (!servicesFor(environment)->recordWav(path, static_cast<int>(duration_ms),
                                           recording)) {
    failServiceResult(result, 8);
    return;
  }
  storeCanonical<int32_t>(result, 8, recording.stream.sample_rate);
  storeCanonical<int32_t>(result, 12, recording.stream.channel_count);
  storeCanonical<int32_t>(result, 16, recording.stream.device_id);
  storeCanonical<int64_t>(result, 24, recording.stream.frames_transferred);
  storeCanonical<double>(result, 32, recording.peak);
  storeCanonical<double>(result, 40, recording.rms);
  if (!lowerStringAt(environment, recording.path.c_str(), result, 48))
    trapInvalidReturnArea(environment);
}

void nativeCameraEnumerate(wasm_exec_env_t environment,
                           uint32_t result_offset) {
  uint8_t *result = serviceResultArea(environment, result_offset, 12);
  if (!result)
    return;
  std::vector<hardware::CameraInfo> cameras;
  if (!servicesFor(environment)->enumerateCameras(cameras) ||
      cameras.size() > 32) {
    failServiceResult(result, 4);
    return;
  }
  const uint32_t count = static_cast<uint32_t>(cameras.size());
  uint32_t pointer = 4;
  uint8_t *records = reinterpret_cast<uint8_t *>(1);
  if (count) {
    records = allocateGuestRecord(environment, count * 32, 4, pointer);
    if (!records)
      return;
  }
  for (uint32_t index = 0; index < count; ++index) {
    uint8_t *camera = records + index * 32;
    if (!lowerStringAt(environment, cameras[index].id.c_str(), camera, 0)) {
      trapInvalidReturnArea(environment);
      return;
    }
    camera[8] = static_cast<uint8_t>(cameras[index].facing);
    storeCanonical<int32_t>(camera, 12, cameras[index].sensor_orientation);
    storeCanonical<int32_t>(camera, 16, cameras[index].hardware_level);
    camera[20] = cameras[index].flash_available;
    storeCanonical<int32_t>(camera, 24, cameras[index].max_jpeg_width);
    storeCanonical<int32_t>(camera, 28, cameras[index].max_jpeg_height);
  }
  storeCanonical(result, 4, pointer);
  storeCanonical(result, 8, count);
}

void nativeCameraSetTorch(wasm_exec_env_t environment, uint32_t id_offset,
                          uint32_t id_length, uint32_t enabled,
                          uint32_t result_offset) {
  std::string id;
  const bool arguments = guestString(environment, id_offset, id_length, id, 128);
  writeResult(environment, result_offset,
              arguments && servicesFor(environment) &&
                  servicesFor(environment)->setTorch(id, enabled != 0),
              !servicesFor(environment) ? serviceAccessError(environment)
              : arguments               ? WitError::Io
                                        : WitError::InvalidArgument);
}

void nativeCameraCapture(wasm_exec_env_t environment, uint32_t id_offset,
                         uint32_t id_length, uint32_t path_offset,
                         uint32_t path_length, uint32_t width, uint32_t height,
                         uint32_t flash, uint32_t timeout_ms,
                         uint32_t result_offset) {
  uint8_t *result = serviceResultArea(environment, result_offset, 32);
  if (!result)
    return;
  std::string id;
  std::string path;
  if (!guestString(environment, id_offset, id_length, id, 128) ||
      !guestString(environment, path_offset, path_length, path, 4096)) {
    failServiceResult(result, 8, WitError::InvalidArgument);
    return;
  }
  hardware::PhotoResult photo;
  if (!servicesFor(environment)->captureJpeg(
          id, path, photo, static_cast<int>(width), static_cast<int>(height),
          flash != 0, static_cast<int>(timeout_ms))) {
    failServiceResult(result, 8);
    return;
  }
  if (!lowerStringAt(environment, photo.path.c_str(), result, 8)) {
    trapInvalidReturnArea(environment);
    return;
  }
  storeCanonical<int32_t>(result, 16, photo.width);
  storeCanonical<int32_t>(result, 20, photo.height);
  storeCanonical<uint64_t>(result, 24, photo.byte_count);
}

void nativeBattery(wasm_exec_env_t environment, uint32_t result_offset) {
  uint8_t *result = serviceResultArea(environment, result_offset, 28);
  if (!result)
    return;
  hardware::BatterySnapshot snapshot;
  if (!servicesFor(environment)->queryBattery(snapshot)) {
    failServiceResult(result, 4);
    return;
  }
  lowerBattery(snapshot, result + 4);
}

void nativeBatteryEvent(wasm_exec_env_t environment, uint32_t timeout_ms,
                        uint32_t result_offset) {
  uint8_t *result = serviceResultArea(environment, result_offset, 32);
  if (!result)
    return;
  hardware::BatterySnapshot snapshot;
  const int changed = servicesFor(environment)->waitForBatteryEvent(
      static_cast<int>(timeout_ms), snapshot);
  if (changed < 0) {
    failServiceResult(result, 4);
  } else if (changed > 0) {
    result[4] = 1;
    lowerBattery(snapshot, result + 8);
  }
}

void nativePowerSetInteractive(wasm_exec_env_t environment, uint32_t enabled,
                               uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && services->setInteractive(enabled != 0),
              services ? WitError::Io : serviceAccessError(environment));
}

void nativePowerAcquireWakeLock(wasm_exec_env_t environment,
                                uint32_t name_offset, uint32_t name_length,
                                uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  std::string name;
  const bool arguments =
      guestString(environment, name_offset, name_length, name, 128);
  writeResult(environment, result_offset,
              services && arguments && services->acquireWakeLock(name),
              !services   ? serviceAccessError(environment)
              : arguments ? WitError::Io
                          : WitError::InvalidArgument);
}

void nativePowerReleaseWakeLock(wasm_exec_env_t environment,
                                uint32_t name_offset, uint32_t name_length,
                                uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  std::string name;
  const bool arguments =
      guestString(environment, name_offset, name_length, name, 128);
  writeResult(environment, result_offset,
              services && arguments && services->releaseWakeLock(name),
              !services   ? serviceAccessError(environment)
              : arguments ? WitError::Io
                          : WitError::InvalidArgument);
}

void nativePowerEnableAutoSuspend(wasm_exec_env_t environment,
                                  uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && services->enableAutoSuspend(),
              services ? WitError::Io : serviceAccessError(environment));
}

void nativePowerDisableAutoSuspend(wasm_exec_env_t environment,
                                   uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && services->disableAutoSuspend(),
              services ? WitError::Io : serviceAccessError(environment));
}

void nativePowerScheduleRtcWake(wasm_exec_env_t environment,
                                uint32_t delay_seconds,
                                uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && services->scheduleRtcWake(delay_seconds),
              services ? WitError::Io : serviceAccessError(environment));
}

void nativePowerClearRtcWake(wasm_exec_env_t environment,
                             uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && services->clearRtcWake(),
              services ? WitError::Io : serviceAccessError(environment));
}

void nativePowerSuspend(wasm_exec_env_t environment, uint32_t timeout_ms,
                        uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && services->suspend(timeout_ms),
              services ? WitError::Io : serviceAccessError(environment));
}

void nativeFlipState(wasm_exec_env_t environment, uint32_t result_offset) {
  uint8_t *result = serviceResultArea(environment, result_offset, 2);
  if (result)
    result[1] = static_cast<uint8_t>(servicesFor(environment)->queryFlipState());
}

void nativeVibrate(wasm_exec_env_t environment, uint32_t duration_ms,
                   uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && services->vibrate(duration_ms),
              services ? WitError::Io : serviceAccessError(environment));
}

void nativeVibrationStop(wasm_exec_env_t environment, uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && services->stopVibration(),
              services ? WitError::Io : serviceAccessError(environment));
}

uint32_t nativeAmplitudeControl(wasm_exec_env_t environment) {
  device::ServiceProvider *services = servicesFor(environment);
  return services && services->supportsAmplitudeControl();
}

void nativeVibrationSetAmplitude(wasm_exec_env_t environment,
                                 uint32_t amplitude,
                                 uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && amplitude <= 255 &&
                  services->setVibrationAmplitude(amplitude),
              !services      ? serviceAccessError(environment)
              : amplitude > 255 ? WitError::InvalidArgument
                                : WitError::Io);
}

void nativeWifiStatus(wasm_exec_env_t environment, uint32_t result_offset) {
  uint8_t *result = serviceResultArea(environment, result_offset, 40);
  if (!result)
    return;
  network::WifiStatus status;
  if (!servicesFor(environment)->wifiStatus(status)) {
    failServiceResult(result, 4);
    return;
  }
  if (!lowerStringAt(environment, status.state.c_str(), result, 4) ||
      !lowerStringAt(environment, status.ssid.c_str(), result, 12) ||
      !lowerStringAt(environment, status.bssid.c_str(), result, 20) ||
      !lowerStringAt(environment, status.ip_address.c_str(), result, 28)) {
    trapInvalidReturnArea(environment);
    return;
  }
  storeCanonical<int32_t>(result, 36, status.network_id);
}

void nativeWifiScan(wasm_exec_env_t environment, uint32_t wait_ms,
                    uint32_t result_offset) {
  uint8_t *result = serviceResultArea(environment, result_offset, 12);
  if (!result)
    return;
  std::vector<network::WifiAccessPoint> access_points;
  if (!servicesFor(environment)->wifiScan(access_points,
                                          static_cast<int>(wait_ms)) ||
      access_points.size() > 256) {
    failServiceResult(result, 4);
    return;
  }
  const uint32_t count = static_cast<uint32_t>(access_points.size());
  uint32_t pointer = 4;
  uint8_t *records = reinterpret_cast<uint8_t *>(1);
  if (count) {
    records = allocateGuestRecord(environment, count * 32, 4, pointer);
    if (!records)
      return;
  }
  for (uint32_t index = 0; index < count; ++index) {
    uint8_t *access_point = records + index * 32;
    if (!lowerStringAt(environment, access_points[index].bssid.c_str(),
                       access_point, 0) ||
        !lowerStringAt(environment, access_points[index].flags.c_str(),
                       access_point, 16) ||
        !lowerStringAt(environment, access_points[index].ssid.c_str(),
                       access_point, 24)) {
      trapInvalidReturnArea(environment);
      return;
    }
    storeCanonical<int32_t>(access_point, 8,
                            access_points[index].frequency_mhz);
    storeCanonical<int32_t>(access_point, 12,
                            access_points[index].signal_dbm);
  }
  storeCanonical(result, 4, pointer);
  storeCanonical(result, 8, count);
}

void nativeWifiNetworks(wasm_exec_env_t environment, uint32_t result_offset) {
  uint8_t *result = serviceResultArea(environment, result_offset, 12);
  if (!result)
    return;
  std::vector<network::WifiNetwork> networks;
  if (!servicesFor(environment)->wifiListNetworks(networks) ||
      networks.size() > 256) {
    failServiceResult(result, 4);
    return;
  }
  const uint32_t count = static_cast<uint32_t>(networks.size());
  uint32_t pointer = 4;
  uint8_t *records = reinterpret_cast<uint8_t *>(1);
  if (count) {
    records = allocateGuestRecord(environment, count * 28, 4, pointer);
    if (!records)
      return;
  }
  for (uint32_t index = 0; index < count; ++index) {
    uint8_t *network = records + index * 28;
    storeCanonical<int32_t>(network, 0, networks[index].id);
    if (!lowerStringAt(environment, networks[index].ssid.c_str(), network, 4) ||
        !lowerStringAt(environment, networks[index].bssid.c_str(), network,
                       12) ||
        !lowerStringAt(environment, networks[index].flags.c_str(), network,
                       20)) {
      trapInvalidReturnArea(environment);
      return;
    }
  }
  storeCanonical(result, 4, pointer);
  storeCanonical(result, 8, count);
}

void nativeWifiConnect(wasm_exec_env_t environment, uint32_t ssid_offset,
                       uint32_t ssid_length, uint32_t security,
                       uint32_t credential_offset, uint32_t credential_length,
                       uint32_t result_offset) {
  uint8_t *result = serviceResultArea(environment, result_offset, 8);
  if (!result)
    return;
  std::string ssid;
  std::string credential;
  if (security > static_cast<uint32_t>(network::WifiSecurity::WpaPsk) ||
      !guestString(environment, ssid_offset, ssid_length, ssid, 32) ||
      !guestString(environment, credential_offset, credential_length,
                   credential, 64)) {
    failServiceResult(result, 4, WitError::InvalidArgument);
    return;
  }
  int network_id = -1;
  if (!servicesFor(environment)->wifiConnect(
          ssid, static_cast<network::WifiSecurity>(security), credential,
          network_id)) {
    failServiceResult(result, 4);
    return;
  }
  storeCanonical<int32_t>(result, 4, network_id);
}

void nativeWifiDisconnect(wasm_exec_env_t environment, uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && services->wifiDisconnect(),
              services ? WitError::Io : serviceAccessError(environment));
}

void nativeWifiReconnect(wasm_exec_env_t environment, uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && services->wifiReconnect(),
              services ? WitError::Io : serviceAccessError(environment));
}

void nativeWifiForget(wasm_exec_env_t environment, uint32_t network_id,
                      uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && services->wifiForget(static_cast<int>(network_id)),
              services ? WitError::Io : serviceAccessError(environment));
}

void nativeWifiSaveConfiguration(wasm_exec_env_t environment,
                                 uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && services->wifiSaveConfiguration(),
              services ? WitError::Io : serviceAccessError(environment));
}

void nativeIpStatus(wasm_exec_env_t environment, uint32_t result_offset) {
  uint8_t *result = serviceResultArea(environment, result_offset, 48);
  if (!result)
    return;
  network::IpConfiguration configuration;
  if (!servicesFor(environment)->ipStatus(configuration)) {
    failServiceResult(result, 4);
    return;
  }
  if (!lowerStringAt(environment, configuration.interface_name.c_str(), result,
                     4) ||
      !lowerStringAt(environment, configuration.address.c_str(), result, 12) ||
      !lowerStringAt(environment, configuration.gateway.c_str(), result, 24) ||
      !lowerStringAt(environment, configuration.dns1.c_str(), result, 32) ||
      !lowerStringAt(environment, configuration.dns2.c_str(), result, 40)) {
    trapInvalidReturnArea(environment);
    return;
  }
  storeCanonical<uint32_t>(result, 20, configuration.prefix_length);
}

void nativeIpUseDhcp(wasm_exec_env_t environment, uint32_t timeout_ms,
                     uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && services->ipUseDhcp(static_cast<int>(timeout_ms)),
              services ? WitError::Io : serviceAccessError(environment));
}

void nativeIpUseStatic(wasm_exec_env_t environment,
                       uint32_t interface_offset, uint32_t interface_length,
                       uint32_t address_offset, uint32_t address_length,
                       uint32_t prefix_length, uint32_t gateway_offset,
                       uint32_t gateway_length, uint32_t dns1_offset,
                       uint32_t dns1_length, uint32_t dns2_offset,
                       uint32_t dns2_length, uint32_t result_offset) {
  network::IpConfiguration configuration;
  configuration.prefix_length = prefix_length;
  const bool arguments =
      guestString(environment, interface_offset, interface_length,
                  configuration.interface_name, 64) &&
      guestString(environment, address_offset, address_length,
                  configuration.address, 64) &&
      guestString(environment, gateway_offset, gateway_length,
                  configuration.gateway, 64) &&
      guestString(environment, dns1_offset, dns1_length, configuration.dns1,
                  64) &&
      guestString(environment, dns2_offset, dns2_length, configuration.dns2,
                  64);
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && arguments && services->ipUseStatic(configuration),
              !services   ? serviceAccessError(environment)
              : arguments ? WitError::Io
                          : WitError::InvalidArgument);
}

void lowerBluetoothScan(wasm_exec_env_t environment, uint32_t duration_ms,
                        uint32_t result_offset, bool low_energy) {
  uint8_t *result = serviceResultArea(environment, result_offset, 12);
  if (!result)
    return;
  std::vector<network::BluetoothDevice> devices;
  const bool success =
      low_energy
          ? servicesFor(environment)->bluetoothLeScan(
                devices, static_cast<int>(duration_ms))
          : servicesFor(environment)->bluetoothClassicScan(
                devices, static_cast<int>(duration_ms));
  if (!success || devices.size() > 256) {
    failServiceResult(result, 4);
    return;
  }
  const uint32_t count = static_cast<uint32_t>(devices.size());
  uint32_t pointer = 4;
  uint8_t *records = reinterpret_cast<uint8_t *>(1);
  if (count) {
    records = allocateGuestRecord(environment, count * 36, 4, pointer);
    if (!records)
      return;
  }
  for (uint32_t index = 0; index < count; ++index) {
    uint8_t *record = records + index * 36;
    if (!lowerStringAt(environment, devices[index].address.c_str(), record, 0) ||
        !lowerStringAt(environment, devices[index].name.c_str(), record, 8)) {
      trapInvalidReturnArea(environment);
      return;
    }
    storeCanonical<int32_t>(record, 16, devices[index].rssi);
    storeCanonical<uint32_t>(record, 20, devices[index].device_class);
    storeCanonical<int32_t>(record, 24, devices[index].device_type);
    uint32_t advertising_pointer = 1;
    if (!devices[index].advertising_data.empty()) {
      advertising_pointer = guestRealloc(
          environment, 0, 0, 1,
          static_cast<uint32_t>(devices[index].advertising_data.size()));
      uint8_t *advertising = appMutableArray<uint8_t>(
          environment, advertising_pointer,
          static_cast<uint32_t>(devices[index].advertising_data.size()), 65536);
      if (!advertising_pointer || !advertising) {
        trapInvalidReturnArea(environment);
        return;
      }
      std::memcpy(advertising, devices[index].advertising_data.data(),
                  devices[index].advertising_data.size());
    }
    storeCanonical(record, 28, advertising_pointer);
    storeCanonical<uint32_t>(
        record, 32,
        static_cast<uint32_t>(devices[index].advertising_data.size()));
  }
  storeCanonical(result, 4, pointer);
  storeCanonical(result, 8, count);
}

void nativeBluetoothClassicScan(wasm_exec_env_t environment,
                                uint32_t duration_ms,
                                uint32_t result_offset) {
  lowerBluetoothScan(environment, duration_ms, result_offset, false);
}

void nativeBluetoothLeScan(wasm_exec_env_t environment, uint32_t duration_ms,
                           uint32_t result_offset) {
  lowerBluetoothScan(environment, duration_ms, result_offset, true);
}

void nativeBluetoothEnable(wasm_exec_env_t environment, uint32_t timeout_ms,
                           uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && services->bluetoothEnable(timeout_ms),
              services ? WitError::Io : serviceAccessError(environment));
}

void nativeBluetoothDisable(wasm_exec_env_t environment, uint32_t timeout_ms,
                            uint32_t result_offset) {
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && services->bluetoothDisable(timeout_ms),
              services ? WitError::Io : serviceAccessError(environment));
}

void nativeBluetoothPair(wasm_exec_env_t environment, uint32_t address_offset,
                         uint32_t address_length, uint32_t transport,
                         uint32_t result_offset) {
  std::string address;
  const bool arguments =
      guestString(environment, address_offset, address_length, address, 32) &&
      transport <= static_cast<uint32_t>(network::BluetoothTransport::LowEnergy);
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(
      environment, result_offset,
      services && arguments &&
          services->bluetoothPair(
              address, static_cast<network::BluetoothTransport>(transport)),
      !services   ? serviceAccessError(environment)
      : arguments ? WitError::Io
                  : WitError::InvalidArgument);
}

void nativeBluetoothUnpair(wasm_exec_env_t environment,
                           uint32_t address_offset, uint32_t address_length,
                           uint32_t result_offset) {
  std::string address;
  const bool arguments =
      guestString(environment, address_offset, address_length, address, 32);
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(environment, result_offset,
              services && arguments && services->bluetoothUnpair(address),
              !services   ? serviceAccessError(environment)
              : arguments ? WitError::Io
                          : WitError::InvalidArgument);
}

void nativeBluetoothCancelPairing(wasm_exec_env_t environment,
                                  uint32_t address_offset,
                                  uint32_t address_length,
                                  uint32_t result_offset) {
  std::string address;
  const bool arguments =
      guestString(environment, address_offset, address_length, address, 32);
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(
      environment, result_offset,
      services && arguments && services->bluetoothCancelPairing(address),
      !services   ? serviceAccessError(environment)
      : arguments ? WitError::Io
                  : WitError::InvalidArgument);
}

void nativeBluetoothProfile(wasm_exec_env_t environment,
                            uint32_t address_offset, uint32_t address_length,
                            uint32_t profile, uint32_t result_offset,
                            bool connect) {
  std::string address;
  const bool arguments =
      guestString(environment, address_offset, address_length, address, 32) &&
      profile <= 2;
  device::ServiceProvider *services = servicesFor(environment);
  const auto native_profile = static_cast<network::BluetoothProfile>(
      profile == 0 ? 0x03 : profile == 1 ? 0x05 : 0x06);
  const bool success =
      services && arguments &&
      (connect ? services->bluetoothProfileConnect(address, native_profile)
               : services->bluetoothProfileDisconnect(address, native_profile));
  writeResult(environment, result_offset, success,
              !services   ? serviceAccessError(environment)
              : arguments ? WitError::Io
                          : WitError::InvalidArgument);
}

void nativeBluetoothProfileConnect(wasm_exec_env_t environment,
                                   uint32_t address_offset,
                                   uint32_t address_length, uint32_t profile,
                                   uint32_t result_offset) {
  nativeBluetoothProfile(environment, address_offset, address_length, profile,
                         result_offset, true);
}

void nativeBluetoothProfileDisconnect(wasm_exec_env_t environment,
                                      uint32_t address_offset,
                                      uint32_t address_length,
                                      uint32_t profile,
                                      uint32_t result_offset) {
  nativeBluetoothProfile(environment, address_offset, address_length, profile,
                         result_offset, false);
}

void nativeBluetoothProfileCycle(wasm_exec_env_t environment,
                                 uint32_t address_offset,
                                 uint32_t address_length, uint32_t profile,
                                 uint32_t hold_ms, uint32_t result_offset) {
  std::string address;
  const bool arguments =
      guestString(environment, address_offset, address_length, address, 32) &&
      profile <= 2;
  device::ServiceProvider *services = servicesFor(environment);
  const auto native_profile = static_cast<network::BluetoothProfile>(
      profile == 0 ? 0x03 : profile == 1 ? 0x05 : 0x06);
  writeResult(
      environment, result_offset,
      services && arguments && services->bluetoothProfileConnectionCycle(
                                  address, native_profile, hold_ms),
      !services   ? serviceAccessError(environment)
      : arguments ? WitError::Io
                  : WitError::InvalidArgument);
}

void nativeBluetoothLeCycle(wasm_exec_env_t environment,
                            uint32_t address_offset, uint32_t address_length,
                            uint32_t hold_ms, uint32_t timeout_ms,
                            uint32_t result_offset) {
  std::string address;
  const bool arguments =
      guestString(environment, address_offset, address_length, address, 32);
  device::ServiceProvider *services = servicesFor(environment);
  writeResult(
      environment, result_offset,
      services && arguments && services->bluetoothLeConnectionCycle(
                                  address, hold_ms, timeout_ms),
      !services   ? serviceAccessError(environment)
      : arguments ? WitError::Io
                  : WitError::InvalidArgument);
}

void nativeModemSnapshot(wasm_exec_env_t environment, uint32_t timeout_ms,
                         uint32_t result_offset) {
  uint8_t *result = serviceResultArea(environment, result_offset, 216);
  if (!result)
    return;
  modem::ModemSnapshot value;
  if (!servicesFor(environment)->modemSnapshot(value, timeout_ms) ||
      value.requests.size() > 256) {
    failServiceResult(result, 4);
    return;
  }
  uint8_t *snapshot = result + 4;
  snapshot[0] = value.service_connected;
  storeCanonical<int32_t>(snapshot, 4, value.radio_state);
  if (!lowerStringAt(environment, value.baseband_version.c_str(), snapshot, 8) ||
      !lowerStringAt(environment, value.identity.imei.c_str(), snapshot, 16) ||
      !lowerStringAt(environment, value.identity.imei_software_version.c_str(),
                     snapshot, 24) ||
      !lowerStringAt(environment, value.identity.esn.c_str(), snapshot, 32) ||
      !lowerStringAt(environment, value.identity.meid.c_str(), snapshot, 40) ||
      !lowerStringAt(environment, value.network_operator.long_name.c_str(),
                     snapshot, 148) ||
      !lowerStringAt(environment, value.network_operator.short_name.c_str(),
                     snapshot, 156) ||
      !lowerStringAt(environment, value.network_operator.numeric.c_str(),
                     snapshot, 164) ||
      !lowerStringAt(environment, value.logical_modem_uuid.c_str(), snapshot,
                     196)) {
    trapInvalidReturnArea(environment);
    return;
  }
  storeCanonical<int32_t>(snapshot, 48, value.sim.card_state);
  storeCanonical<int32_t>(snapshot, 52, value.sim.universal_pin_state);
  storeCanonical<int32_t>(snapshot, 56, value.sim.application_count);
  const int signal[] = {
      value.signal.gsm_strength,
      value.signal.gsm_bit_error_rate,
      value.signal.cdma_dbm,
      value.signal.cdma_ecio,
      value.signal.evdo_dbm,
      value.signal.evdo_ecio,
      value.signal.evdo_snr,
      value.signal.lte_strength,
      value.signal.lte_rsrp,
      value.signal.lte_rsrq,
      value.signal.lte_rssnr,
      value.signal.lte_cqi,
      value.signal.lte_timing_advance,
      value.signal.tdscdma_rscp,
  };
  for (size_t index = 0; index < std::size(signal); ++index)
    storeCanonical<int32_t>(snapshot, 60 + index * 4, signal[index]);
  const modem::RegistrationStatus registrations[] = {
      value.voice_registration, value.data_registration};
  for (size_t index = 0; index < std::size(registrations); ++index) {
    const size_t offset = 116 + index * 16;
    storeCanonical<int32_t>(snapshot, offset, registrations[index].state);
    storeCanonical<int32_t>(snapshot, offset + 4,
                            registrations[index].radio_technology);
    storeCanonical<int32_t>(snapshot, offset + 8,
                            registrations[index].denial_reason);
    storeCanonical<int32_t>(snapshot, offset + 12,
                            registrations[index].max_data_calls);
  }
  storeCanonical<int32_t>(snapshot, 172, value.preferred_network_type);
  storeCanonical<int32_t>(snapshot, 176, value.voice_radio_technology);
  storeCanonical<int32_t>(snapshot, 180, value.current_call_count);
  storeCanonical<int32_t>(snapshot, 184, value.data_call_count);
  storeCanonical<int32_t>(snapshot, 188, value.hardware_config_count);
  storeCanonical<uint32_t>(snapshot, 192, value.radio_access_family);
  const uint32_t request_count = static_cast<uint32_t>(value.requests.size());
  uint32_t request_pointer = 4;
  uint8_t *requests = reinterpret_cast<uint8_t *>(1);
  if (request_count) {
    requests =
        allocateGuestRecord(environment, request_count * 16, 4, request_pointer);
    if (!requests)
      return;
  }
  for (uint32_t index = 0; index < request_count; ++index) {
    uint8_t *record = requests + index * 16;
    if (!lowerStringAt(environment, value.requests[index].operation.c_str(),
                       record, 0)) {
      trapInvalidReturnArea(environment);
      return;
    }
    storeCanonical<int32_t>(record, 8, value.requests[index].error);
    record[12] = value.requests[index].timed_out;
  }
  storeCanonical(snapshot, 204, request_pointer);
  storeCanonical(snapshot, 208, request_count);
}

void nativeRadioPower(wasm_exec_env_t environment, uint32_t enabled,
                      uint32_t timeout_ms, uint32_t result_offset) {
  uint8_t *result = serviceResultArea(environment, result_offset, 20);
  if (!result)
    return;
  modem::ModemRequestStatus status;
  if (!servicesFor(environment)->setRadioPower(enabled != 0, status,
                                               timeout_ms)) {
    failServiceResult(result, 4);
    return;
  }
  if (!lowerStringAt(environment, status.operation.c_str(), result, 4)) {
    trapInvalidReturnArea(environment);
    return;
  }
  storeCanonical<int32_t>(result, 12, status.error);
  result[16] = status.timed_out;
}

void nativeCodec(wasm_exec_env_t environment, uint32_t width, uint32_t height,
                 uint32_t frame_count, uint32_t timeout_ms,
                 uint32_t result_offset) {
  uint8_t *result = serviceResultArea(environment, result_offset, 56);
  if (!result)
    return;
  hardware::CodecResult codec;
  if (!servicesFor(environment)->testH264RoundTrip(
          static_cast<int>(width), static_cast<int>(height),
          static_cast<int>(frame_count), codec, static_cast<int>(timeout_ms))) {
    failServiceResult(result, 8);
    return;
  }
  if (!lowerStringAt(environment, codec.encoder_name.c_str(), result, 8) ||
      !lowerStringAt(environment, codec.decoder_name.c_str(), result, 16)) {
    trapInvalidReturnArea(environment);
    return;
  }
  result[24] = codec.encoder_hardware_accelerated;
  result[25] = codec.decoder_hardware_accelerated;
  storeCanonical<int32_t>(result, 28, codec.width);
  storeCanonical<int32_t>(result, 32, codec.height);
  storeCanonical<int32_t>(result, 36, codec.input_frames);
  storeCanonical<int32_t>(result, 40, codec.output_buffers);
  storeCanonical<int32_t>(result, 44, codec.decoded_frames);
  storeCanonical<uint64_t>(result, 48, codec.encoded_bytes);
}

NativeSymbol kRuntimeSymbols[] = {
    {"abi-version", reinterpret_cast<void *>(nativeAbiVersion), "()i", nullptr},
    {"wall-clock-minutes", reinterpret_cast<void *>(nativeWallClockMinutes),
     "()i", nullptr},
    {"log", reinterpret_cast<void *>(nativeLog), "(iii)", nullptr},
};

NativeSymbol kGraphicsSymbols[] = {
    {"surface-size", reinterpret_cast<void *>(nativeSurfaceSize), "(i)",
     nullptr},
    {"surface-format", reinterpret_cast<void *>(nativeSurfaceFormat), "()i",
     nullptr},
    {"supported-texture-formats",
     reinterpret_cast<void *>(nativeSupportedTextureFormats), "()i", nullptr},
    {"graphics-limits", reinterpret_cast<void *>(nativeGraphicsLimits), "(i)",
     nullptr},
    {"texture-set", reinterpret_cast<void *>(nativeTextureSet), "(iiiiiiiiiii)",
     nullptr},
    {"texture-free", reinterpret_cast<void *>(nativeTextureFree), "(ii)",
     nullptr},
    {"submit", reinterpret_cast<void *>(nativeSubmit), "(iiiiiiii)", nullptr},
};

NativeSymbol kGlesSymbols[] = {
    {"get-capabilities", reinterpret_cast<void *>(nativeGlesCapabilities),
     "(i)", nullptr},
    {"buffer-set", reinterpret_cast<void *>(nativeGlesBufferSet), "(iiiiii)",
     nullptr},
    {"buffer-write", reinterpret_cast<void *>(nativeGlesBufferWrite), "(iiiii)",
     nullptr},
    {"buffer-free", reinterpret_cast<void *>(nativeGlesBufferFree), "(ii)",
     nullptr},
    {"shader-set", reinterpret_cast<void *>(nativeGlesShaderSet), "(iiiii)",
     nullptr},
    {"shader-free", reinterpret_cast<void *>(nativeGlesShaderFree), "(ii)",
     nullptr},
    {"program-set", reinterpret_cast<void *>(nativeGlesProgramSet), "(iiii)",
     nullptr},
    {"program-free", reinterpret_cast<void *>(nativeGlesProgramFree), "(ii)",
     nullptr},
    {"attribute-location",
     reinterpret_cast<void *>(nativeGlesAttributeLocation), "(iii)i", nullptr},
    {"uniform-location", reinterpret_cast<void *>(nativeGlesUniformLocation),
     "(iii)i", nullptr},
    {"submit", reinterpret_cast<void *>(nativeGlesSubmit), "(iiiii)", nullptr},
};

NativeSymbol kDeviceSymbols[] = {
    {"get-descriptor", reinterpret_cast<void *>(nativeDeviceDescriptor), "(i)",
     nullptr},
    {"get-capability", reinterpret_cast<void *>(nativeDeviceCapability), "(i)i",
     nullptr},
};

NativeSymbol kAudioSymbols[] = {
    {"play-tone", reinterpret_cast<void *>(nativeAudioPlayTone), "(Fifii)",
     reinterpret_cast<void *>(8)},
    {"record-wav", reinterpret_cast<void *>(nativeAudioRecord), "(iiii)",
     serviceAttachment(8, WasmServicePermission::AudioCapture)},
    {"last-error", reinterpret_cast<void *>(nativeServiceMessage), "(i)",
     nullptr},
};

NativeSymbol kCameraSymbols[] = {
    {"enumerate", reinterpret_cast<void *>(nativeCameraEnumerate), "(i)",
     serviceAttachment(4, WasmServicePermission::Camera)},
    {"set-torch", reinterpret_cast<void *>(nativeCameraSetTorch), "(iiii)",
     serviceAttachment(1, WasmServicePermission::Camera)},
    {"capture-jpeg", reinterpret_cast<void *>(nativeCameraCapture),
     "(iiiiiiiii)", serviceAttachment(8, WasmServicePermission::Camera)},
    {"last-error", reinterpret_cast<void *>(nativeServiceMessage), "(i)",
     serviceAttachment(0, WasmServicePermission::Camera)},
};

NativeSymbol kPowerSymbols[] = {
    {"query-battery", reinterpret_cast<void *>(nativeBattery), "(i)",
     reinterpret_cast<void *>(4)},
    {"wait-for-battery-event", reinterpret_cast<void *>(nativeBatteryEvent),
     "(ii)", reinterpret_cast<void *>(4)},
    {"set-interactive", reinterpret_cast<void *>(nativePowerSetInteractive),
     "(ii)",
     serviceAttachment(1, WasmServicePermission::Power)},
    {"acquire-wake-lock",
     reinterpret_cast<void *>(nativePowerAcquireWakeLock),
     "(iii)", serviceAttachment(1, WasmServicePermission::Power)},
    {"release-wake-lock",
     reinterpret_cast<void *>(nativePowerReleaseWakeLock),
     "(iii)", serviceAttachment(1, WasmServicePermission::Power)},
    {"enable-auto-suspend",
     reinterpret_cast<void *>(nativePowerEnableAutoSuspend),
     "(i)", serviceAttachment(1, WasmServicePermission::Power)},
    {"disable-auto-suspend",
     reinterpret_cast<void *>(nativePowerDisableAutoSuspend),
     "(i)", serviceAttachment(1, WasmServicePermission::Power)},
    {"schedule-rtc-wake",
     reinterpret_cast<void *>(nativePowerScheduleRtcWake), "(ii)",
     serviceAttachment(1, WasmServicePermission::Power)},
    {"clear-rtc-wake", reinterpret_cast<void *>(nativePowerClearRtcWake),
     "(i)", serviceAttachment(1, WasmServicePermission::Power)},
    {"suspend", reinterpret_cast<void *>(nativePowerSuspend),
     "(ii)", serviceAttachment(1, WasmServicePermission::Power)},
    {"query-flip-state", reinterpret_cast<void *>(nativeFlipState), "(i)",
     serviceAttachment(1, WasmServicePermission::Power)},
    {"last-error", reinterpret_cast<void *>(nativeServiceMessage), "(i)",
     nullptr},
};

NativeSymbol kVibratorSymbols[] = {
    {"vibrate", reinterpret_cast<void *>(nativeVibrate),
     "(ii)", reinterpret_cast<void *>(1)},
    {"stop", reinterpret_cast<void *>(nativeVibrationStop), "(i)",
     reinterpret_cast<void *>(1)},
    {"supports-amplitude-control",
     reinterpret_cast<void *>(nativeAmplitudeControl), "()i", nullptr},
    {"set-amplitude", reinterpret_cast<void *>(nativeVibrationSetAmplitude),
     "(ii)",
     reinterpret_cast<void *>(1)},
    {"last-error", reinterpret_cast<void *>(nativeServiceMessage), "(i)",
     nullptr},
};

NativeSymbol kWifiSymbols[] = {
    {"get-status", reinterpret_cast<void *>(nativeWifiStatus), "(i)",
     serviceAttachment(4, WasmServicePermission::Wifi)},
    {"scan", reinterpret_cast<void *>(nativeWifiScan), "(ii)",
     serviceAttachment(4, WasmServicePermission::Wifi)},
    {"list-networks", reinterpret_cast<void *>(nativeWifiNetworks), "(i)",
     serviceAttachment(4, WasmServicePermission::Wifi)},
    {"connect", reinterpret_cast<void *>(nativeWifiConnect), "(iiiiii)",
     serviceAttachment(4, WasmServicePermission::Wifi)},
    {"disconnect", reinterpret_cast<void *>(nativeWifiDisconnect), "(i)",
     serviceAttachment(1, WasmServicePermission::Wifi)},
    {"reconnect", reinterpret_cast<void *>(nativeWifiReconnect), "(i)",
     serviceAttachment(1, WasmServicePermission::Wifi)},
    {"forget", reinterpret_cast<void *>(nativeWifiForget),
     "(ii)", serviceAttachment(1, WasmServicePermission::Wifi)},
    {"save-configuration",
     reinterpret_cast<void *>(nativeWifiSaveConfiguration),
     "(i)", serviceAttachment(1, WasmServicePermission::Wifi)},
    {"last-error", reinterpret_cast<void *>(nativeServiceMessage), "(i)",
     serviceAttachment(0, WasmServicePermission::Wifi)},
};

NativeSymbol kIpSymbols[] = {
    {"get-status", reinterpret_cast<void *>(nativeIpStatus), "(i)",
     serviceAttachment(4, WasmServicePermission::Wifi)},
    {"use-dhcp", reinterpret_cast<void *>(nativeIpUseDhcp),
     "(ii)", serviceAttachment(1, WasmServicePermission::Wifi)},
    {"use-static", reinterpret_cast<void *>(nativeIpUseStatic),
     "(iiiiiiiiiiii)", serviceAttachment(1, WasmServicePermission::Wifi)},
    {"last-error", reinterpret_cast<void *>(nativeServiceMessage), "(i)",
     serviceAttachment(0, WasmServicePermission::Wifi)},
};

NativeSymbol kBluetoothSymbols[] = {
    {"enable", reinterpret_cast<void *>(nativeBluetoothEnable),
     "(ii)", serviceAttachment(1, WasmServicePermission::Bluetooth)},
    {"disable", reinterpret_cast<void *>(nativeBluetoothDisable),
     "(ii)", serviceAttachment(1, WasmServicePermission::Bluetooth)},
    {"classic-scan", reinterpret_cast<void *>(nativeBluetoothClassicScan),
     "(ii)", serviceAttachment(4, WasmServicePermission::Bluetooth)},
    {"le-scan", reinterpret_cast<void *>(nativeBluetoothLeScan), "(ii)",
     serviceAttachment(4, WasmServicePermission::Bluetooth)},
    {"pair", reinterpret_cast<void *>(nativeBluetoothPair),
     "(iiii)", serviceAttachment(1, WasmServicePermission::Bluetooth)},
    {"unpair", reinterpret_cast<void *>(nativeBluetoothUnpair),
     "(iii)", serviceAttachment(1, WasmServicePermission::Bluetooth)},
    {"cancel-pairing", reinterpret_cast<void *>(nativeBluetoothCancelPairing),
     "(iii)", serviceAttachment(1, WasmServicePermission::Bluetooth)},
    {"profile-connect", reinterpret_cast<void *>(nativeBluetoothProfileConnect),
     "(iiii)", serviceAttachment(1, WasmServicePermission::Bluetooth)},
    {"profile-disconnect",
     reinterpret_cast<void *>(nativeBluetoothProfileDisconnect),
     "(iiii)", serviceAttachment(1, WasmServicePermission::Bluetooth)},
    {"profile-connection-cycle",
     reinterpret_cast<void *>(nativeBluetoothProfileCycle),
     "(iiiii)", serviceAttachment(1, WasmServicePermission::Bluetooth)},
    {"le-connection-cycle",
     reinterpret_cast<void *>(nativeBluetoothLeCycle),
     "(iiiii)", serviceAttachment(1, WasmServicePermission::Bluetooth)},
    {"last-error", reinterpret_cast<void *>(nativeServiceMessage), "(i)",
     serviceAttachment(0, WasmServicePermission::Bluetooth)},
};

NativeSymbol kModemSymbols[] = {
    {"query-snapshot", reinterpret_cast<void *>(nativeModemSnapshot),
     "(ii)", serviceAttachment(4, WasmServicePermission::Modem)},
    {"set-radio-power", reinterpret_cast<void *>(nativeRadioPower), "(iii)",
     serviceAttachment(4, WasmServicePermission::Modem)},
    {"last-error", reinterpret_cast<void *>(nativeServiceMessage), "(i)",
     serviceAttachment(0, WasmServicePermission::Modem)},
};

NativeSymbol kCodecSymbols[] = {
    {"test-h264-round-trip", reinterpret_cast<void *>(nativeCodec),
     "(iiiii)", reinterpret_cast<void *>(8)},
    {"last-error", reinterpret_cast<void *>(nativeServiceMessage), "(i)",
     nullptr},
};

NativeSymbol kStorageSymbols[] = {
    {"kv-get", reinterpret_cast<void *>(nativeKvGet), "(iii)", nullptr},
    {"kv-set", reinterpret_cast<void *>(nativeKvSet), "(iiiii)", nullptr},
    {"kv-delete", reinterpret_cast<void *>(nativeKvDelete), "(iii)", nullptr},
    {"kv-clear", reinterpret_cast<void *>(nativeKvClear), "(i)", nullptr},
    {"database-execute", reinterpret_cast<void *>(nativeDatabaseExecute),
     "(iiiii)", nullptr},
    {"database-prepare", reinterpret_cast<void *>(nativeDatabasePrepare),
     "(iiiii)", nullptr},
    {"statement-bind-null", reinterpret_cast<void *>(nativeStatementBindNull),
     "(iii)", nullptr},
    {"statement-bind-integer",
     reinterpret_cast<void *>(nativeStatementBindInteger), "(iiIi)", nullptr},
    {"statement-bind-float", reinterpret_cast<void *>(nativeStatementBindFloat),
     "(iiFi)", nullptr},
    {"statement-bind-text", reinterpret_cast<void *>(nativeStatementBindText),
     "(iiiii)", nullptr},
    {"statement-bind-blob", reinterpret_cast<void *>(nativeStatementBindBlob),
     "(iiiii)", nullptr},
    {"statement-step", reinterpret_cast<void *>(nativeStatementStep), "(ii)",
     nullptr},
    {"statement-column-count",
     reinterpret_cast<void *>(nativeStatementColumnCount), "(ii)", nullptr},
    {"statement-column-kind",
     reinterpret_cast<void *>(nativeStatementColumnKind), "(iii)", nullptr},
    {"statement-column-integer",
     reinterpret_cast<void *>(nativeStatementColumnInteger), "(iii)", nullptr},
    {"statement-column-float",
     reinterpret_cast<void *>(nativeStatementColumnFloat), "(iii)", nullptr},
    {"statement-column-text",
     reinterpret_cast<void *>(nativeStatementColumnText), "(iii)", nullptr},
    {"statement-column-blob",
     reinterpret_cast<void *>(nativeStatementColumnBlob), "(iii)", nullptr},
    {"statement-finish", reinterpret_cast<void *>(nativeStatementFinish),
     "(ii)", nullptr},
};

NativeSymbol kDeviceStorageSymbols[] = {
    {"enumerate-files", reinterpret_cast<void *>(nativeDeviceStorageEnumerate),
     "(ii)", nullptr},
    {"read-file", reinterpret_cast<void *>(nativeDeviceStorageRead), "(iiii)",
     nullptr},
    {"write-file", reinterpret_cast<void *>(nativeDeviceStorageWrite),
     "(iiiiiii)", nullptr},
    {"delete-file", reinterpret_cast<void *>(nativeDeviceStorageDelete),
     "(iiii)", nullptr},
    {"free-space", reinterpret_cast<void *>(nativeDeviceStorageSpace), "(ii)",
     reinterpret_cast<void *>(0)},
    {"used-space", reinterpret_cast<void *>(nativeDeviceStorageSpace), "(ii)",
     reinterpret_cast<void *>(1)},
};

struct WitNativeInterface {
  const char *name;
  NativeSymbol *symbols;
  uint32_t count;
};

WitNativeInterface kOptionalInterfaces[] = {
    {kAudioInterface, kAudioSymbols,
     static_cast<uint32_t>(std::size(kAudioSymbols))},
    {kCameraInterface, kCameraSymbols,
     static_cast<uint32_t>(std::size(kCameraSymbols))},
    {kPowerInterface, kPowerSymbols,
     static_cast<uint32_t>(std::size(kPowerSymbols))},
    {kVibratorInterface, kVibratorSymbols,
     static_cast<uint32_t>(std::size(kVibratorSymbols))},
    {kWifiInterface, kWifiSymbols,
     static_cast<uint32_t>(std::size(kWifiSymbols))},
    {kIpInterface, kIpSymbols, static_cast<uint32_t>(std::size(kIpSymbols))},
    {kBluetoothInterface, kBluetoothSymbols,
     static_cast<uint32_t>(std::size(kBluetoothSymbols))},
    {kModemInterface, kModemSymbols,
     static_cast<uint32_t>(std::size(kModemSymbols))},
    {kCodecInterface, kCodecSymbols,
     static_cast<uint32_t>(std::size(kCodecSymbols))},
    {kStorageInterface, kStorageSymbols,
     static_cast<uint32_t>(std::size(kStorageSymbols))},
    {kDeviceStorageInterface, kDeviceStorageSymbols,
     static_cast<uint32_t>(std::size(kDeviceStorageSymbols))},
};

uint32_t gRuntimeReferences = 0;

bool acquireRuntime(std::string &error) {
  if (gRuntimeReferences == 0) {
    RuntimeInitArgs arguments = {};
    arguments.mem_alloc_type = Alloc_With_System_Allocator;
    arguments.native_module_name = kRuntimeInterface;
    arguments.native_symbols = kRuntimeSymbols;
    arguments.n_native_symbols =
        static_cast<uint32_t>(std::size(kRuntimeSymbols));
    if (!wasm_runtime_full_init(&arguments)) {
      error = "WAMR initialization failed";
      return false;
    }
    bool registered = wasm_runtime_register_natives(
                          kGraphicsInterface, kGraphicsSymbols,
                          static_cast<uint32_t>(std::size(kGraphicsSymbols))) &&
                      wasm_runtime_register_natives(
                          kGlesInterface, kGlesSymbols,
                          static_cast<uint32_t>(std::size(kGlesSymbols))) &&
                      wasm_runtime_register_natives(
                          kDeviceInterface, kDeviceSymbols,
                          static_cast<uint32_t>(std::size(kDeviceSymbols)));
    for (const WitNativeInterface &interface : kOptionalInterfaces) {
      registered =
          registered && wasm_runtime_register_natives(
                            interface.name, interface.symbols, interface.count);
    }
    if (!registered) {
      error = "WAMR WIT interface registration failed";
      wasm_runtime_destroy();
      return false;
    }
  }
  ++gRuntimeReferences;
  return true;
}

void releaseRuntime() {
  if (gRuntimeReferences == 0)
    return;
  if (--gRuntimeReferences == 0)
    wasm_runtime_destroy();
}

class NamespacedGraphicsHost final : public GraphicsHost {
public:
  explicit NamespacedGraphicsHost(GraphicsHost &host) : host_(host) {}
  ~NamespacedGraphicsHost() override { reset(); }

  uint32_t width() const override { return host_.width(); }
  uint32_t height() const override { return host_.height(); }
  uint32_t surfaceFormat() const override { return host_.surfaceFormat(); }
  uint32_t supportedTextureFormats() const override {
    return host_.supportedTextureFormats();
  }

  bool setTexture(uint32_t texture, uint32_t format, uint32_t x, uint32_t y,
                  uint32_t width, uint32_t height, uint32_t row_stride,
                  uint32_t flags, const uint8_t *pixels,
                  size_t pixel_bytes) override {
    auto *found = findHandle(textures_, texture);
    const bool inserted = found == nullptr;
    if (inserted) {
      if (x != 0 || y != 0)
        return false;
      const uint32_t host_texture = nextHostHandle();
      if (host_texture == 0)
        return false;
      textures_.emplace_back(texture, host_texture);
      found = &textures_.back();
    }
    if (host_.setTexture(found->second, format, x, y, width, height, row_stride,
                         flags, pixels, pixel_bytes)) {
      return true;
    }
    if (inserted)
      textures_.pop_back();
    return false;
  }

  bool freeTexture(uint32_t texture) override {
    for (size_t index = 0; index < textures_.size(); ++index) {
      if (textures_[index].first != texture)
        continue;
      if (!host_.freeTexture(textures_[index].second))
        return false;
      textures_.erase(textures_.begin() + index);
      return true;
    }
    return true;
  }

  bool submit(const OosGfxVertex *vertices, size_t vertex_count,
              const uint16_t *indices, size_t index_count,
              const OosGfxDrawCommand *commands, size_t command_count,
              uint32_t clear_rgba) override {
    translated_draw_commands_.clear();
    translated_draw_commands_.reserve(command_count);
    for (size_t index = 0; index < command_count; ++index) {
      const auto *texture = findHandle(textures_, commands[index].texture);
      if (!texture)
        return false;
      translated_draw_commands_.push_back(commands[index]);
      translated_draw_commands_.back().texture = texture->second;
    }
    return host_.submit(vertices, vertex_count, indices, index_count,
                        translated_draw_commands_.empty()
                            ? nullptr
                            : translated_draw_commands_.data(),
                        translated_draw_commands_.size(), clear_rgba);
  }

  bool glesCapabilities(OosGlesCapabilities &result) override {
    return host_.glesCapabilities(result);
  }

  bool setGlesBuffer(uint32_t buffer, uint32_t size, uint32_t usage,
                     const uint8_t *data, size_t data_size) override {
    auto *found = findHandle(buffers_, buffer);
    const bool inserted = found == nullptr;
    if (inserted) {
      buffers_.emplace_back(buffer, nextHostHandle());
      found = &buffers_.back();
    }
    if (host_.setGlesBuffer(found->second, size, usage, data, data_size))
      return true;
    if (inserted)
      buffers_.pop_back();
    return false;
  }

  bool writeGlesBuffer(uint32_t buffer, uint32_t offset, const uint8_t *data,
                       size_t data_size) override {
    const auto *found = findHandle(buffers_, buffer);
    return found &&
           host_.writeGlesBuffer(found->second, offset, data, data_size);
  }

  bool freeGlesBuffer(uint32_t buffer) override {
    return freeHandle(buffers_, buffer, [this](uint32_t host) {
      return host_.freeGlesBuffer(host);
    });
  }

  bool setGlesShader(uint32_t shader, uint32_t stage, const char *source,
                     size_t source_size) override {
    auto *found = findHandle(shaders_, shader);
    const bool inserted = found == nullptr;
    if (inserted) {
      shaders_.emplace_back(shader, nextHostHandle());
      found = &shaders_.back();
    }
    if (host_.setGlesShader(found->second, stage, source, source_size))
      return true;
    if (inserted)
      shaders_.pop_back();
    return false;
  }

  bool freeGlesShader(uint32_t shader) override {
    return freeHandle(shaders_, shader, [this](uint32_t host) {
      return host_.freeGlesShader(host);
    });
  }

  bool setGlesProgram(uint32_t program, uint32_t vertex_shader,
                      uint32_t fragment_shader) override {
    const auto *vertex = findHandle(shaders_, vertex_shader);
    const auto *fragment = findHandle(shaders_, fragment_shader);
    if (!vertex || !fragment)
      return false;
    auto *found = findHandle(programs_, program);
    const bool inserted = found == nullptr;
    if (inserted) {
      programs_.emplace_back(program, nextHostHandle());
      found = &programs_.back();
    }
    if (host_.setGlesProgram(found->second, vertex->second, fragment->second))
      return true;
    if (inserted)
      programs_.pop_back();
    return false;
  }

  bool freeGlesProgram(uint32_t program) override {
    return freeHandle(programs_, program, [this](uint32_t host) {
      return host_.freeGlesProgram(host);
    });
  }

  int32_t glesAttributeLocation(uint32_t program, const char *name,
                                size_t name_size) override {
    const auto *found = findHandle(programs_, program);
    return found ? host_.glesAttributeLocation(found->second, name, name_size)
                 : -1;
  }

  int32_t glesUniformLocation(uint32_t program, const char *name,
                              size_t name_size) override {
    const auto *found = findHandle(programs_, program);
    return found ? host_.glesUniformLocation(found->second, name, name_size)
                 : -1;
  }

  bool submitGles(const OosGlesCommand *commands, size_t command_count,
                  const uint32_t *data, size_t data_words) override {
    if (!commands || command_count < 2 ||
        command_count > OOS_GLES_MAX_COMMANDS ||
        data_words > OOS_GLES_MAX_COMMAND_DATA_WORDS ||
        (data_words != 0 && !data))
      return false;
    translated_gles_commands_.assign(commands, commands + command_count);
    for (OosGlesCommand &command : translated_gles_commands_) {
      const HandleMap *resources = nullptr;
      uint32_t argument = 0;
      switch (command.opcode) {
      case OOS_GLES_USE_PROGRAM:
        resources = &programs_;
        break;
      case OOS_GLES_BIND_TEXTURE:
        resources = &textures_;
        argument = 1;
        break;
      case OOS_GLES_BIND_VERTEX_BUFFER:
      case OOS_GLES_BIND_INDEX_BUFFER:
        resources = &buffers_;
        break;
      default:
        continue;
      }
      const auto *found = findHandle(*resources, command.args[argument]);
      if (!found)
        return false;
      command.args[argument] = found->second;
    }
    return host_.submitGles(translated_gles_commands_.data(),
                            translated_gles_commands_.size(), data, data_words);
  }

  void reset() {
    for (const auto &program : programs_)
      host_.freeGlesProgram(program.second);
    for (const auto &shader : shaders_)
      host_.freeGlesShader(shader.second);
    for (const auto &buffer : buffers_)
      host_.freeGlesBuffer(buffer.second);
    for (const auto &texture : textures_)
      host_.freeTexture(texture.second);
    programs_.clear();
    shaders_.clear();
    buffers_.clear();
    textures_.clear();
    translated_draw_commands_.clear();
    translated_gles_commands_.clear();
  }

private:
  using HandleMap = std::vector<std::pair<uint32_t, uint32_t>>;

  static std::pair<uint32_t, uint32_t> *findHandle(HandleMap &handles,
                                                   uint32_t guest) {
    for (auto &handle : handles) {
      if (handle.first == guest)
        return &handle;
    }
    return nullptr;
  }

  static const std::pair<uint32_t, uint32_t> *
  findHandle(const HandleMap &handles, uint32_t guest) {
    for (const auto &handle : handles) {
      if (handle.first == guest)
        return &handle;
    }
    return nullptr;
  }

  template <typename Free>
  static bool freeHandle(HandleMap &handles, uint32_t guest, Free free_host) {
    for (size_t index = 0; index < handles.size(); ++index) {
      if (handles[index].first != guest)
        continue;
      if (!free_host(handles[index].second))
        return false;
      handles.erase(handles.begin() + index);
      return true;
    }
    return true;
  }

  static uint32_t nextHostHandle() {
    static std::atomic<uint32_t> next{1};
    uint32_t handle = next.fetch_add(1, std::memory_order_relaxed);
    if (handle == 0)
      handle = next.fetch_add(1, std::memory_order_relaxed);
    return handle;
  }

  GraphicsHost &host_;
  HandleMap textures_;
  HandleMap buffers_;
  HandleMap shaders_;
  HandleMap programs_;
  std::vector<OosGfxDrawCommand> translated_draw_commands_;
  std::vector<OosGlesCommand> translated_gles_commands_;
};

class MappedModule {
public:
  ~MappedModule() { reset(); }

  bool open(const char *path, std::string &error) {
    reset();
    const int file = ::open(path, O_RDONLY | O_CLOEXEC);
    if (file < 0) {
      error = std::string("open ") + path + ": " + std::strerror(errno);
      return false;
    }
    struct stat status = {};
    if (fstat(file, &status) != 0 || status.st_size <= 0 ||
        static_cast<uint64_t>(status.st_size) > kMaxModuleBytes) {
      error = std::string("invalid module file: ") + path;
      ::close(file);
      return false;
    }
    size_ = static_cast<size_t>(status.st_size);
    void *mapping =
        mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE, file, 0);
    const int saved_errno = errno;
    ::close(file);
    if (mapping == MAP_FAILED) {
      data_ = nullptr;
      size_ = 0;
      error = std::string("mmap ") + path + ": " + std::strerror(saved_errno);
      return false;
    }
    data_ = static_cast<uint8_t *>(mapping);
    return true;
  }

  void reset() {
    if (data_)
      munmap(data_, size_);
    data_ = nullptr;
    size_ = 0;
  }

  uint8_t *data() const { return data_; }
  uint32_t size() const { return static_cast<uint32_t>(size_); }

private:
  uint8_t *data_ = nullptr;
  size_t size_ = 0;
};

} // namespace

class WasmApp::Impl {
public:
  Impl(GraphicsHost &graphics, device::Device *device, WasmAppOptions options)
      : graphics(graphics), options(std::move(options)) {
    if (!this->options.data_directory.empty()) {
      app_storage =
          std::make_unique<storage::AppStorage>(this->options.data_directory);
    }
    if (!this->options.internal_media_directory.empty() &&
        !this->options.removable_media_directory.empty()) {
      device_storage = std::make_unique<storage::DeviceStorageService>(
          this->options.internal_media_directory,
          this->options.removable_media_directory);
    }
    host = {&this->graphics,
            device,
            &services,
            app_storage.get(),
            device_storage.get(),
            this->options.service_permission_mask,
            this->options.enforce_service_permissions};
  }

  ~Impl() { shutdown(); }

  bool initializeRuntime() {
    if (runtime_initialized)
      return true;
    if (!acquireRuntime(error))
      return false;
    runtime_initialized = true;
    return true;
  }

  bool call(const char *name, uint32_t argc, uint32_t *argv) {
    wasm_function_inst_t function =
        wasm_runtime_lookup_function(instance, name);
    if (!function) {
      error = std::string("missing WASM export: ") + name;
      return false;
    }
    if (!wasm_runtime_call_wasm(environment, function, argc, argv)) {
      const char *exception = wasm_runtime_get_exception(instance);
      error = std::string(name) +
              " failed: " + (exception ? exception : "unknown WASM exception");
      return false;
    }
    return true;
  }

  bool callResult(const char *name, uint32_t argc, uint32_t *argv) {
    if (!call(name, argc, argv))
      return false;
    const uint32_t result_offset = argv ? argv[0] : 0;
    const uint8_t *result = appArray<uint8_t>(environment, result_offset, 2, 2);
    if (!result || result[0] > 1) {
      error = std::string(name) + " returned an invalid WIT result";
      return false;
    }
    if (result[0] == 0)
      return true;
    error = std::string(name) + " returned " + witErrorName(result[1]);
    return false;
  }

  void shutdown() {
    if (instance && environment) {
      wasm_function_inst_t function =
          wasm_runtime_lookup_function(instance, kLifecycleShutdown);
      if (function)
        wasm_runtime_call_wasm(environment, function, 0, nullptr);
    }
    if (environment) {
      wasm_runtime_destroy_exec_env(environment);
      environment = nullptr;
    }
    if (instance) {
      wasm_runtime_deinstantiate(instance);
      instance = nullptr;
    }
    if (module) {
      wasm_runtime_unload(module);
      module = nullptr;
    }
    graphics.reset();
    module_bytes.reset();
    initialized = false;
    if (runtime_initialized) {
      releaseRuntime();
      runtime_initialized = false;
    }
  }

  NamespacedGraphicsHost graphics;
  std::unique_ptr<device::ServiceProvider> services;
  std::unique_ptr<storage::AppStorage> app_storage;
  std::unique_ptr<storage::DeviceStorageService> device_storage;
  AppHostContext host;
  WasmAppOptions options;
  MappedModule module_bytes;
  wasm_module_t module = nullptr;
  wasm_module_inst_t instance = nullptr;
  wasm_exec_env_t environment = nullptr;
  std::string error;
  bool runtime_initialized = false;
  bool initialized = false;
};

WasmApp::WasmApp(GraphicsHost &graphics, WasmAppOptions options)
    : impl_(std::make_unique<Impl>(graphics, nullptr, options)) {}

WasmApp::WasmApp(GraphicsHost &graphics, device::Device &device,
                 WasmAppOptions options)
    : impl_(std::make_unique<Impl>(graphics, &device, options)) {}

WasmApp::~WasmApp() = default;

bool WasmApp::load(const char *path) {
  impl_->shutdown();
  impl_->error.clear();
  if (!path || path[0] == '\0') {
    impl_->error = "WASM app path is empty";
    return false;
  }
  if (impl_->app_storage && !impl_->app_storage->initialize()) {
    impl_->error = "initialize app storage: " + impl_->app_storage->lastError();
    return false;
  }
  if (!impl_->initializeRuntime() ||
      !impl_->module_bytes.open(path, impl_->error)) {
    return false;
  }
  std::array<char, kErrorBufferSize> error_buffer{};
  impl_->module =
      wasm_runtime_load(impl_->module_bytes.data(), impl_->module_bytes.size(),
                        error_buffer.data(), error_buffer.size());
  if (!impl_->module) {
    impl_->error = std::string("load WASM module: ") + error_buffer.data();
    return false;
  }
  impl_->instance = wasm_runtime_instantiate(
      impl_->module, impl_->options.stack_size, impl_->options.heap_size,
      error_buffer.data(), error_buffer.size());
  if (!impl_->instance) {
    impl_->error =
        std::string("instantiate WASM module: ") + error_buffer.data();
    return false;
  }
  wasm_runtime_set_custom_data(impl_->instance, &impl_->host);
  impl_->environment =
      wasm_runtime_create_exec_env(impl_->instance, impl_->options.stack_size);
  if (!impl_->environment) {
    impl_->error = "create WAMR execution environment failed";
    return false;
  }
  return true;
}

bool WasmApp::initialize() {
  if (!loaded()) {
    impl_->error = "WASM app is not loaded";
    return false;
  }
  uint32_t result[1] = {};
  impl_->initialized = impl_->callResult(kLifecycleInit, 0, result);
  return impl_->initialized;
}

bool WasmApp::dispatchKey(const input::KeyEvent &event, int64_t monotonic_us) {
  if (!impl_->initialized)
    return false;
  const uint64_t timestamp = static_cast<uint64_t>(monotonic_us);
  uint32_t arguments[4] = {
      event.code,
      static_cast<uint32_t>(event.action),
      static_cast<uint32_t>(timestamp),
      static_cast<uint32_t>(timestamp >> 32),
  };
  return impl_->call(kLifecycleEvent, std::size(arguments), arguments);
}

bool WasmApp::render(int64_t monotonic_us) {
  if (!impl_->initialized)
    return false;
  const uint64_t timestamp = static_cast<uint64_t>(monotonic_us);
  uint32_t arguments[2] = {
      static_cast<uint32_t>(timestamp),
      static_cast<uint32_t>(timestamp >> 32),
  };
  return impl_->callResult(kLifecycleFrame, std::size(arguments), arguments);
}

void WasmApp::shutdown() { impl_->shutdown(); }

const char *WasmApp::lastError() const { return impl_->error.c_str(); }

bool WasmApp::loaded() const {
  return impl_->instance != nullptr && impl_->environment != nullptr;
}

} // namespace oos::runtime
