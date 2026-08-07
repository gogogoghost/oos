#include "oos/runtime/js_platform_services.h"

#include "oos/apps/app_manifest.h"
#include "oos/apps/permissions.h"
#include "oos/apps/zip_archive.h"
#include "oos/device/device.h"
#include "oos/device/service_provider.h"
#include "oos/hardware/audio_manager.h"
#include "oos/hardware/camera_manager.h"
#include "oos/hardware/codec_manager.h"
#include "oos/hardware/power_manager.h"
#include "oos/media/audio_format.h"
#include "oos/media/media_service.h"
#include "oos/modem/modem_manager.h"
#include "oos/network/bluetooth_manager.h"
#include "oos/network/ip_manager.h"
#include "oos/network/wifi_manager.h"
#include "oos/resources/font_assets.h"
#include "oos/resources/package_assets.h"
#include "oos/runtime/application_context.h"
#include "oos/runtime/graphics_types.h"
#include "oos/services/system_service.h"
#include "oos/storage/app_storage.h"
#include "oos/storage/device_storage.h"
#include "oos/ui/status_bar_appearance.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace oos::runtime {
namespace {

constexpr size_t kMaximumStringBytes = 256 * 1024;
constexpr size_t kMaximumStorageBytes = 64 * 1024 * 1024;
constexpr uint32_t kMaximumWaitMs = 60 * 1000;

ApplicationContext *applicationFor(JSContext *context) {
  auto *host = static_cast<JsPlatformHost *>(JS_GetContextOpaque(context));
  return host ? host->jsApplicationContext() : nullptr;
}

JsPlatformHost *hostFor(JSContext *context) {
  return static_cast<JsPlatformHost *>(JS_GetContextOpaque(context));
}

JSValue throwPlatformError(JSContext *context, const char *code,
                           const std::string &message) {
  JSValue error = JS_NewError(context);
  if (JS_IsException(error))
    return error;
  JS_SetPropertyStr(context, error, "name",
                    JS_NewString(context, "OosPlatformError"));
  JS_SetPropertyStr(context, error, "code", JS_NewString(context, code));
  JS_SetPropertyStr(context, error, "message",
                    JS_NewStringLen(context, message.data(), message.size()));
  return JS_Throw(context, error);
}

JSValue unavailable(JSContext *context, ApplicationContext *application,
                    uint32_t permission, const char *service) {
  if (application && !application->permissionGranted(permission))
    return throwPlatformError(context, "permission-denied",
                              std::string(service) + " permission denied");
  return throwPlatformError(context, "unavailable",
                            std::string(service) + " is unavailable");
}

JSValue failed(JSContext *context, const std::string &message,
               const char *code = "io") {
  return throwPlatformError(context, code,
                            message.empty() ? "platform operation failed"
                                            : message);
}

JSValue runtimeAbiVersion(JSContext *context, JSValueConst, int,
                          JSValueConst *) {
  return JS_NewUint32(context, OOS_WASM_ABI_VERSION);
}

JSValue runtimeMonotonicTime(JSContext *context, JSValueConst, int,
                             JSValueConst *) {
  timespec now = {};
  const uint64_t value = ::clock_gettime(CLOCK_MONOTONIC, &now) == 0
                             ? static_cast<uint64_t>(now.tv_sec) * 1000000ULL +
                                   static_cast<uint64_t>(now.tv_nsec) / 1000ULL
                             : 0;
  return JS_NewBigUint64(context, value);
}

JSValue runtimeWallClockMinutes(JSContext *context, JSValueConst, int,
                                JSValueConst *) {
  const time_t now = std::time(nullptr);
  tm local = {};
  const uint32_t value = localtime_r(&now, &local)
                             ? static_cast<uint32_t>(local.tm_hour * 60 +
                                                     local.tm_min)
                             : 0;
  return JS_NewUint32(context, value);
}

JSValue runtimeWallClockTime(JSContext *context, JSValueConst, int,
                             JSValueConst *) {
  timespec now = {};
  const int64_t value = ::clock_gettime(CLOCK_REALTIME, &now) == 0
                            ? static_cast<int64_t>(now.tv_sec) * 1000LL +
                                  now.tv_nsec / 1000000
                            : 0;
  return JS_NewBigInt64(context, value);
}

JSValue runtimeWakeMainThread(JSContext *context, JSValueConst, int,
                              JSValueConst *) {
  ApplicationContext *application = applicationFor(context);
  const int wake_fd = application ? application->wakeFd() : -1;
  if (wake_fd >= 0) {
    const uint64_t value = 1;
    ssize_t written;
    do {
      written = ::write(wake_fd, &value, sizeof(value));
    } while (written < 0 && errno == EINTR);
  }
  return JS_UNDEFINED;
}

JSValue runtimeRequestExit(JSContext *context, JSValueConst, int,
                           JSValueConst *) {
  JsPlatformHost *host = hostFor(context);
  if (!host || !host->jsRequestExit())
    return unavailable(context, applicationFor(context), 0, "request-exit");
  return JS_UNDEFINED;
}

JSValue runtimeSetStatusBarStyle(JSContext *context, JSValueConst, int argc,
                                 JSValueConst *argv) {
  uint32_t background = 0;
  if (argc < 2 || JS_ToUint32(context, &background, argv[0]) < 0)
    return JS_ThrowTypeError(context, "expected background RGB and darkIcons");
  const int dark = JS_ToBool(context, argv[1]);
  ApplicationContext *application = applicationFor(context);
  auto *status_bar = application ? application->statusBar() : nullptr;
  if (dark < 0 || !status_bar || (background & 0xff000000u) != 0)
    return JS_ThrowRangeError(context,
                              "status bar style is unavailable or invalid");
  status_bar->setStatusBarAppearance({background, dark != 0});
  return JS_UNDEFINED;
}

JSValue runtimeSetSurfaceMode(JSContext *context, JSValueConst, int argc,
                              JSValueConst *argv) {
  const char *mode = argc > 0 ? JS_ToCString(context, argv[0]) : nullptr;
  ApplicationContext *application = applicationFor(context);
  auto *status_bar = application ? application->statusBar() : nullptr;
  const bool normal = mode && std::strcmp(mode, "normal") == 0;
  const bool immersive = mode && std::strcmp(mode, "immersive") == 0;
  const bool success = status_bar && (normal || immersive) &&
                       status_bar->setSurfaceMode(
                           normal ? ui::SurfaceMode::Normal
                                  : ui::SurfaceMode::Immersive);
  if (mode)
    JS_FreeCString(context, mode);
  if (!success)
    return JS_ThrowRangeError(context,
                              "surface mode is unavailable or invalid");
  return JS_UNDEFINED;
}

JSValue runtimeLog(JSContext *context, JSValueConst, int argc,
                   JSValueConst *argv) {
  if (argc < 2)
    return JS_ThrowTypeError(context, "log expects a level and message");
  const char *level = JS_ToCString(context, argv[0]);
  const char *message = JS_ToCString(context, argv[1]);
  const bool valid = level && message &&
                     (std::strcmp(level, "debug") == 0 ||
                      std::strcmp(level, "info") == 0 ||
                      std::strcmp(level, "warn") == 0 ||
                      std::strcmp(level, "error") == 0);
  if (valid)
    std::fprintf(stderr, "[js:%s] %s\n", level, message);
  if (level)
    JS_FreeCString(context, level);
  if (message)
    JS_FreeCString(context, message);
  return valid ? JS_UNDEFINED
               : JS_ThrowTypeError(context, "invalid log level or message");
}

int initializeRuntimeModule(JSContext *context, JSModuleDef *module) {
  struct Export {
    const char *name;
    JSCFunction *function;
    int arguments;
  };
  constexpr Export exports[] = {
      {"abiVersion", runtimeAbiVersion, 0},
      {"wallClockMinutes", runtimeWallClockMinutes, 0},
      {"monotonicTimeUs", runtimeMonotonicTime, 0},
      {"wallClockTimeMs", runtimeWallClockTime, 0},
      {"wakeMainThread", runtimeWakeMainThread, 0},
      {"requestExit", runtimeRequestExit, 0},
      {"setStatusBarStyle", runtimeSetStatusBarStyle, 2},
      {"setSurfaceMode", runtimeSetSurfaceMode, 1},
      {"log", runtimeLog, 2},
  };
  for (const Export &entry : exports) {
    if (JS_SetModuleExport(context, module, entry.name,
                           JS_NewCFunction(context, entry.function, entry.name,
                                           entry.arguments)) < 0)
      return -1;
  }
  return 0;
}

bool stringArgument(JSContext *context, JSValueConst value, std::string &result,
                    size_t maximum = kMaximumStringBytes) {
  size_t size = 0;
  const char *text = JS_ToCStringLen(context, &size, value);
  if (!text)
    return false;
  const bool valid = size <= maximum;
  if (valid)
    result.assign(text, size);
  JS_FreeCString(context, text);
  return valid;
}

bool bytesArgument(JSContext *context, JSValueConst value,
                   const uint8_t *&bytes, size_t &size, JSValue &backing,
                   size_t maximum = kMaximumStorageBytes) {
  bytes = JS_GetArrayBuffer(context, &size, value);
  if (!bytes) {
    size_t offset = 0;
    size_t bytes_per_element = 0;
    backing = JS_GetTypedArrayBuffer(context, value, &offset, &size,
                                     &bytes_per_element);
    size_t backing_size = 0;
    uint8_t *data = JS_IsException(backing)
                        ? nullptr
                        : JS_GetArrayBuffer(context, &backing_size, backing);
    if (!data || offset > backing_size || size > backing_size - offset)
      return false;
    bytes = data + offset;
  }
  return size <= maximum;
}

JSValue newBytes(JSContext *context, const uint8_t *bytes, size_t size) {
  JSValue buffer = JS_NewArrayBufferCopy(context, bytes, size);
  if (JS_IsException(buffer))
    return buffer;
  JSValue arguments[3] = {buffer, JS_UNDEFINED, JS_UNDEFINED};
  JSValue result =
      JS_NewTypedArray(context, 3, arguments, JS_TYPED_ARRAY_UINT8);
  JS_FreeValue(context, buffer);
  return result;
}

bool setString(JSContext *context, JSValue object, const char *name,
               const std::string &value) {
  return JS_SetPropertyStr(
             context, object, name,
             JS_NewStringLen(context, value.data(), value.size())) >= 0;
}

bool setString(JSContext *context, JSValue object, const char *name,
               const char *value) {
  return JS_SetPropertyStr(context, object, name,
                           JS_NewString(context, value ? value : "")) >= 0;
}

bool setI32(JSContext *context, JSValue object, const char *name,
            int32_t value) {
  return JS_SetPropertyStr(context, object, name,
                           JS_NewInt32(context, value)) >= 0;
}

bool setU32(JSContext *context, JSValue object, const char *name,
            uint32_t value) {
  return JS_SetPropertyStr(context, object, name,
                           JS_NewUint32(context, value)) >= 0;
}

bool setI64(JSContext *context, JSValue object, const char *name,
            int64_t value) {
  return JS_SetPropertyStr(context, object, name,
                           JS_NewBigInt64(context, value)) >= 0;
}

bool setU64(JSContext *context, JSValue object, const char *name,
            uint64_t value) {
  return JS_SetPropertyStr(context, object, name,
                           JS_NewBigUint64(context, value)) >= 0;
}

bool setBool(JSContext *context, JSValue object, const char *name, bool value) {
  return JS_SetPropertyStr(context, object, name,
                           JS_NewBool(context, value)) >= 0;
}

bool int32Argument(JSContext *context, JSValueConst value, int32_t &result) {
  return JS_ToInt32(context, &result, value) == 0;
}

bool uint32Argument(JSContext *context, JSValueConst value, uint32_t &result) {
  return JS_ToUint32(context, &result, value) == 0;
}

bool uint64Argument(JSContext *context, JSValueConst value, uint64_t &result) {
  if (JS_IsBigInt(context, value)) {
    size_t size = 0;
    const char *text = JS_ToCStringLen(context, &size, value);
    if (!text)
      return false;
    const auto parsed = std::from_chars(text, text + size, result, 10);
    const bool valid = parsed.ec == std::errc() && parsed.ptr == text + size;
    JS_FreeCString(context, text);
    return valid;
  }
  return JS_ToIndex(context, &result, value) == 0;
}

bool doubleArgument(JSContext *context, JSValueConst value, double &result) {
  return JS_ToFloat64(context, &result, value) == 0 && std::isfinite(result);
}

bool boolArgument(JSContext *context, JSValueConst value, bool &result) {
  const int converted = JS_ToBool(context, value);
  if (converted < 0)
    return false;
  result = converted != 0;
  return true;
}

bool getStringProperty(JSContext *context, JSValueConst object,
                       const char *name, std::string &result,
                       size_t maximum = 4096) {
  JSValue value = JS_GetPropertyStr(context, object, name);
  const bool valid = !JS_IsException(value) &&
                     stringArgument(context, value, result, maximum);
  JS_FreeValue(context, value);
  return valid;
}

bool getU32Property(JSContext *context, JSValueConst object, const char *name,
                    uint32_t &result) {
  JSValue value = JS_GetPropertyStr(context, object, name);
  const bool valid = !JS_IsException(value) &&
                     uint32Argument(context, value, result);
  JS_FreeValue(context, value);
  return valid;
}

device::ServiceProvider *serviceFor(JSContext *context, uint32_t permission,
                                    const char *name, JSValue &failure) {
  ApplicationContext *application = applicationFor(context);
  if (!application || !application->permissionGranted(permission)) {
    failure = unavailable(context, application, permission, name);
    return nullptr;
  }
  device::ServiceProvider *service = application->services();
  if (!service)
    failure = unavailable(context, application, permission, name);
  return service;
}

uint32_t permissionForFeature(uint32_t feature) {
  using Permission = apps::DeviceServicePermission;
  using Feature = device::Feature;
  switch (static_cast<Feature>(feature)) {
  case Feature::AudioCapture:
    return apps::permissionBit(Permission::AudioCapture);
  case Feature::CameraCapture:
  case Feature::Torch:
    return apps::permissionBit(Permission::Camera);
  case Feature::Suspend:
  case Feature::RtcWake:
    return apps::permissionBit(Permission::Power);
  case Feature::Wifi:
  case Feature::IpConfiguration:
    return apps::permissionBit(Permission::Wifi);
  case Feature::BluetoothClassic:
  case Feature::BluetoothLowEnergy:
    return apps::permissionBit(Permission::Bluetooth);
  case Feature::Modem:
    return apps::permissionBit(Permission::Modem);
  default:
    return 0;
  }
}

JSValue jsDeviceDescriptor(JSContext *context, JSValueConst, int,
                           JSValueConst *) {
  ApplicationContext *application = applicationFor(context);
  device::Device *target = application ? application->device() : nullptr;
  const device::DeviceDescriptor empty = {};
  const device::DeviceDescriptor &descriptor = target ? target->descriptor() : empty;
  JSValue result = JS_NewObject(context);
  if (JS_IsException(result))
    return result;
  setString(context, result, "id", descriptor.id);
  setString(context, result, "manufacturer", descriptor.manufacturer);
  setString(context, result, "model", descriptor.model);
  setU32(context, result, "androidApi", descriptor.android_api);
  setU32(context, result, "primaryWidth", descriptor.primary_width);
  setU32(context, result, "primaryHeight", descriptor.primary_height);
  setU32(context, result, "secondaryWidth", descriptor.secondary_width);
  setU32(context, result, "secondaryHeight", descriptor.secondary_height);
  return result;
}

JSValue jsDeviceCapability(JSContext *context, JSValueConst, int argc,
                           JSValueConst *argv) {
  uint32_t feature = 0;
  ApplicationContext *application = applicationFor(context);
  device::Device *target = application ? application->device() : nullptr;
  if (argc < 1 || !uint32Argument(context, argv[0], feature) ||
      feature >= static_cast<uint32_t>(device::Feature::Count))
    return JS_ThrowRangeError(context, "invalid device feature");
  return JS_NewUint32(
      context, target ? static_cast<uint32_t>(target->capability(
                            static_cast<device::Feature>(feature)))
                      : 0);
}

JSValue jsDeviceAccess(JSContext *context, JSValueConst, int argc,
                       JSValueConst *argv) {
  uint32_t feature = 0;
  ApplicationContext *application = applicationFor(context);
  device::Device *target = application ? application->device() : nullptr;
  if (argc < 1 || !uint32Argument(context, argv[0], feature) ||
      feature >= static_cast<uint32_t>(device::Feature::Count))
    return JS_ThrowRangeError(context, "invalid device feature");
  if (!target)
    return JS_NewUint32(context, 0);
  const auto capability =
      target->capability(static_cast<device::Feature>(feature));
  if (capability != device::CapabilityState::Implemented &&
      capability != device::CapabilityState::Validated)
    return JS_NewUint32(context, 0);
  return JS_NewUint32(
      context,
      application->permissionGranted(permissionForFeature(feature)) ? 2 : 1);
}

JSValue jsKvGet(JSContext *context, JSValueConst, int argc,
                JSValueConst *argv) {
  ApplicationContext *application = applicationFor(context);
  storage::AppStorage *storage = application ? application->storage() : nullptr;
  std::string key;
  if (argc < 1 || !stringArgument(context, argv[0], key, 1024))
    return JS_ThrowTypeError(context, "kvGet expects a bounded string key");
  if (!storage)
    return unavailable(context, application, 0, "application storage");
  std::vector<uint8_t> value;
  bool found = false;
  if (!storage->get(key, value, found))
    return failed(context, storage->lastError());
  return found ? newBytes(context, value.data(), value.size()) : JS_NULL;
}

JSValue jsKvSet(JSContext *context, JSValueConst, int argc,
                JSValueConst *argv) {
  ApplicationContext *application = applicationFor(context);
  storage::AppStorage *storage = application ? application->storage() : nullptr;
  std::string key;
  const uint8_t *bytes = nullptr;
  size_t size = 0;
  JSValue backing = JS_UNDEFINED;
  const bool valid = argc >= 2 && stringArgument(context, argv[0], key, 1024) &&
                     bytesArgument(context, argv[1], bytes, size, backing,
                                   4 * 1024 * 1024);
  const bool success = valid && storage &&
                       storage->set(key, size ? bytes : nullptr, size);
  JS_FreeValue(context, backing);
  if (!valid)
    return JS_ThrowTypeError(context, "kvSet expects a key and byte array");
  if (!storage)
    return unavailable(context, application, 0, "application storage");
  if (!success)
    return failed(context, storage->lastError());
  return JS_UNDEFINED;
}

JSValue jsKvDelete(JSContext *context, JSValueConst, int argc,
                   JSValueConst *argv) {
  ApplicationContext *application = applicationFor(context);
  storage::AppStorage *storage = application ? application->storage() : nullptr;
  std::string key;
  if (argc < 1 || !stringArgument(context, argv[0], key, 1024))
    return JS_ThrowTypeError(context, "kvDelete expects a bounded string key");
  if (!storage)
    return unavailable(context, application, 0, "application storage");
  bool removed = false;
  if (!storage->remove(key, removed))
    return failed(context, storage->lastError());
  return JS_NewBool(context, removed);
}

JSValue jsKvClear(JSContext *context, JSValueConst, int, JSValueConst *) {
  ApplicationContext *application = applicationFor(context);
  storage::AppStorage *storage = application ? application->storage() : nullptr;
  if (!storage)
    return unavailable(context, application, 0, "application storage");
  if (!storage->clear())
    return failed(context, storage->lastError());
  return JS_UNDEFINED;
}

enum class DatabaseOperation {
  Execute,
  Prepare,
  BindNull,
  BindInteger,
  BindFloat,
  BindText,
  BindBlob,
  Step,
  ColumnCount,
  ColumnKind,
  ColumnInteger,
  ColumnFloat,
  ColumnText,
  ColumnBlob,
  Finish,
};

JSValue jsDatabase(JSContext *context, JSValueConst, int argc,
                   JSValueConst *argv, int magic) {
  ApplicationContext *application = applicationFor(context);
  storage::AppStorage *storage = application ? application->storage() : nullptr;
  if (!storage)
    return unavailable(context, application, 0, "application storage");
  const auto operation = static_cast<DatabaseOperation>(magic);
  uint32_t handle = 0;
  uint32_t index = 0;
  std::string first;
  std::string second;
  bool success = false;
  switch (operation) {
  case DatabaseOperation::Execute:
  case DatabaseOperation::Prepare: {
    if (argc < 2 || !stringArgument(context, argv[0], first, 255) ||
        !stringArgument(context, argv[1], second, 256 * 1024))
      return JS_ThrowTypeError(context, "database operation expects name and SQL");
    uint32_t result = 0;
    success = operation == DatabaseOperation::Execute
                  ? storage->databaseExecute(first, second, result)
                  : storage->databasePrepare(first, second, result);
    if (!success)
      return failed(context, storage->lastError());
    return JS_NewUint32(context, result);
  }
  case DatabaseOperation::BindNull:
  case DatabaseOperation::BindInteger:
  case DatabaseOperation::BindFloat:
  case DatabaseOperation::BindText:
  case DatabaseOperation::BindBlob:
    if (argc < 2 || !uint32Argument(context, argv[0], handle) ||
        !uint32Argument(context, argv[1], index))
      return JS_ThrowTypeError(context, "statement binding expects handle and index");
    if (operation == DatabaseOperation::BindNull) {
      success = storage->statementBindNull(handle, index);
    } else if (operation == DatabaseOperation::BindInteger) {
      int64_t value = 0;
      if (argc < 3 || JS_ToBigInt64(context, &value, argv[2]) < 0)
        return JS_ThrowTypeError(context, "integer binding expects a bigint");
      success = storage->statementBindInteger(handle, index, value);
    } else if (operation == DatabaseOperation::BindFloat) {
      double value = 0;
      if (argc < 3 || !doubleArgument(context, argv[2], value))
        return JS_ThrowTypeError(context, "float binding expects a finite number");
      success = storage->statementBindFloat(handle, index, value);
    } else if (operation == DatabaseOperation::BindText) {
      if (argc < 3 || !stringArgument(context, argv[2], first))
        return JS_ThrowTypeError(context, "text binding expects a string");
      success = storage->statementBindText(handle, index, first);
    } else {
      const uint8_t *bytes = nullptr;
      size_t size = 0;
      JSValue backing = JS_UNDEFINED;
      if (argc < 3 || !bytesArgument(context, argv[2], bytes, size, backing,
                                     4 * 1024 * 1024)) {
        JS_FreeValue(context, backing);
        return JS_ThrowTypeError(context, "blob binding expects bytes");
      }
      success = storage->statementBindBlob(handle, index,
                                           size ? bytes : nullptr, size);
      JS_FreeValue(context, backing);
    }
    break;
  case DatabaseOperation::Step: {
    if (argc < 1 || !uint32Argument(context, argv[0], handle))
      return JS_ThrowTypeError(context, "statementStep expects a handle");
    storage::SqlRowState state = storage::SqlRowState::Done;
    if (!storage->statementStep(handle, state))
      return failed(context, storage->lastError());
    return JS_NewUint32(context, static_cast<uint32_t>(state));
  }
  case DatabaseOperation::ColumnCount: {
    if (argc < 1 || !uint32Argument(context, argv[0], handle))
      return JS_ThrowTypeError(context, "statementColumnCount expects a handle");
    uint32_t count = 0;
    if (!storage->statementColumnCount(handle, count))
      return failed(context, storage->lastError());
    return JS_NewUint32(context, count);
  }
  case DatabaseOperation::ColumnKind: {
    if (argc < 2 || !uint32Argument(context, argv[0], handle) ||
        !uint32Argument(context, argv[1], index))
      return JS_ThrowTypeError(context, "column operation expects handle and index");
    storage::SqlValueKind kind = storage::SqlValueKind::Null;
    if (!storage->statementColumnKind(handle, index, kind))
      return failed(context, storage->lastError());
    return JS_NewUint32(context, static_cast<uint32_t>(kind));
  }
  case DatabaseOperation::ColumnInteger: {
    if (argc < 2 || !uint32Argument(context, argv[0], handle) ||
        !uint32Argument(context, argv[1], index))
      return JS_ThrowTypeError(context, "column operation expects handle and index");
    int64_t value = 0;
    if (!storage->statementColumnInt64(handle, index, value))
      return failed(context, storage->lastError());
    return JS_NewBigInt64(context, value);
  }
  case DatabaseOperation::ColumnFloat: {
    if (argc < 2 || !uint32Argument(context, argv[0], handle) ||
        !uint32Argument(context, argv[1], index))
      return JS_ThrowTypeError(context, "column operation expects handle and index");
    double value = 0;
    if (!storage->statementColumnDouble(handle, index, value))
      return failed(context, storage->lastError());
    return JS_NewFloat64(context, value);
  }
  case DatabaseOperation::ColumnText: {
    if (argc < 2 || !uint32Argument(context, argv[0], handle) ||
        !uint32Argument(context, argv[1], index))
      return JS_ThrowTypeError(context, "column operation expects handle and index");
    std::string value;
    if (!storage->statementColumnText(handle, index, value))
      return failed(context, storage->lastError());
    return JS_NewStringLen(context, value.data(), value.size());
  }
  case DatabaseOperation::ColumnBlob: {
    if (argc < 2 || !uint32Argument(context, argv[0], handle) ||
        !uint32Argument(context, argv[1], index))
      return JS_ThrowTypeError(context, "column operation expects handle and index");
    std::vector<uint8_t> value;
    if (!storage->statementColumnBlob(handle, index, value))
      return failed(context, storage->lastError());
    return newBytes(context, value.data(), value.size());
  }
  case DatabaseOperation::Finish:
    if (argc < 1 || !uint32Argument(context, argv[0], handle))
      return JS_ThrowTypeError(context, "statementFinish expects a handle");
    success = storage->statementFinish(handle);
    break;
  }
  if (!success)
    return failed(context, storage->lastError());
  return JS_UNDEFINED;
}

bool deviceStorageVolume(JSContext *context, JSValueConst value,
                         storage::DeviceStorageVolume &volume) {
  uint32_t raw = 0;
  if (!uint32Argument(context, value, raw) || raw > 1)
    return false;
  volume = static_cast<storage::DeviceStorageVolume>(raw);
  return true;
}

storage::DeviceStorageService *deviceStorageFor(
    JSContext *context, uint32_t permission, JSValue &failure) {
  ApplicationContext *application = applicationFor(context);
  if (!application || !application->permissionGranted(permission)) {
    failure = unavailable(context, application, permission, "device storage");
    return nullptr;
  }
  storage::DeviceStorageService *service = application->deviceStorage();
  if (!service)
    failure = unavailable(context, application, permission, "device storage");
  return service;
}

JSValue jsDeviceStorageList(JSContext *context, JSValueConst, int argc,
                            JSValueConst *argv) {
  storage::DeviceStorageVolume volume;
  if (argc < 1 || !deviceStorageVolume(context, argv[0], volume))
    return JS_ThrowRangeError(context, "invalid device storage volume");
  JSValue failure = JS_UNDEFINED;
  auto *service = deviceStorageFor(
      context,
      apps::permissionBit(apps::DeviceServicePermission::DeviceStorageRead),
      failure);
  if (!service)
    return failure;
  std::vector<storage::DeviceStorageEntry> entries;
  if (!service->list(volume, entries))
    return failed(context, service->lastError());
  JSValue result = JS_NewArray(context);
  for (uint32_t index = 0; index < entries.size(); ++index) {
    JSValue item = JS_NewObject(context);
    setString(context, item, "path", entries[index].path);
    setU64(context, item, "size", entries[index].size);
    setI64(context, item, "lastModifiedMs", entries[index].last_modified_ms);
    JS_SetPropertyUint32(context, result, index, item);
  }
  return result;
}

JSValue jsDeviceStorageRead(JSContext *context, JSValueConst, int argc,
                            JSValueConst *argv) {
  storage::DeviceStorageVolume volume;
  std::string path;
  if (argc < 2 || !deviceStorageVolume(context, argv[0], volume) ||
      !stringArgument(context, argv[1], path, 4096) ||
      !apps::validPackagePath(path) || path.back() == '/')
    return JS_ThrowTypeError(context, "invalid device storage path");
  JSValue failure = JS_UNDEFINED;
  auto *service = deviceStorageFor(
      context,
      apps::permissionBit(apps::DeviceServicePermission::DeviceStorageRead),
      failure);
  if (!service)
    return failure;
  std::vector<uint8_t> bytes;
  if (!service->read(volume, path, bytes) || bytes.size() > kMaximumStorageBytes)
    return failed(context, service->lastError());
  return newBytes(context, bytes.data(), bytes.size());
}

JSValue jsDeviceStorageWrite(JSContext *context, JSValueConst, int argc,
                             JSValueConst *argv) {
  storage::DeviceStorageVolume volume;
  std::string path;
  uint32_t mode = 0;
  const uint8_t *bytes = nullptr;
  size_t size = 0;
  JSValue backing = JS_UNDEFINED;
  const bool valid =
      argc >= 4 && deviceStorageVolume(context, argv[0], volume) &&
      stringArgument(context, argv[1], path, 4096) &&
      apps::validPackagePath(path) && path.back() != '/' &&
      uint32Argument(context, argv[2], mode) && mode <= 2 &&
      bytesArgument(context, argv[3], bytes, size, backing);
  if (!valid) {
    JS_FreeValue(context, backing);
    return JS_ThrowTypeError(context, "invalid device storage write arguments");
  }
  const uint32_t permission = apps::permissionBit(
      mode == 0 ? apps::DeviceServicePermission::DeviceStorageCreate
                : apps::DeviceServicePermission::DeviceStorageWrite);
  JSValue failure = JS_UNDEFINED;
  auto *service = deviceStorageFor(context, permission, failure);
  const bool success = service && service->write(
                                      volume, path,
                                      static_cast<storage::DeviceStorageWriteMode>(mode),
                                      size ? bytes : nullptr, size);
  JS_FreeValue(context, backing);
  if (!service)
    return failure;
  if (!success)
    return failed(context, service->lastError());
  return JS_UNDEFINED;
}

JSValue jsDeviceStorageDelete(JSContext *context, JSValueConst, int argc,
                              JSValueConst *argv) {
  storage::DeviceStorageVolume volume;
  std::string path;
  if (argc < 2 || !deviceStorageVolume(context, argv[0], volume) ||
      !stringArgument(context, argv[1], path, 4096) ||
      !apps::validPackagePath(path) || path.back() == '/')
    return JS_ThrowTypeError(context, "invalid device storage path");
  JSValue failure = JS_UNDEFINED;
  auto *service = deviceStorageFor(
      context,
      apps::permissionBit(apps::DeviceServicePermission::DeviceStorageWrite),
      failure);
  if (!service)
    return failure;
  bool removed = false;
  if (!service->remove(volume, path, removed))
    return failed(context, service->lastError());
  return JS_NewBool(context, removed);
}

JSValue jsDeviceStorageSpace(JSContext *context, JSValueConst, int argc,
                             JSValueConst *argv, int magic) {
  storage::DeviceStorageVolume volume;
  if (argc < 1 || !deviceStorageVolume(context, argv[0], volume))
    return JS_ThrowRangeError(context, "invalid device storage volume");
  JSValue failure = JS_UNDEFINED;
  auto *service = deviceStorageFor(
      context,
      apps::permissionBit(apps::DeviceServicePermission::DeviceStorageRead),
      failure);
  if (!service)
    return failure;
  uint64_t bytes = 0;
  const bool success = magic == 0 ? service->freeSpace(volume, bytes)
                                  : service->usedSpace(volume, bytes);
  return success ? JS_NewBigUint64(context, bytes)
                 : failed(context, service->lastError());
}

JSValue jsFontLoad(JSContext *context, JSValueConst, int argc,
                   JSValueConst *argv) {
  ApplicationContext *application = applicationFor(context);
  auto *service = application ? application->fontAssets() : nullptr;
  uint32_t role = 0;
  if (argc < 1 || !uint32Argument(context, argv[0], role) || role > 3)
    return JS_ThrowRangeError(context, "invalid font role");
  if (!service)
    return unavailable(context, application, 0, "font assets");
  uint64_t size = 0;
  auto status = service->fileSize(static_cast<resources::FontRole>(role), size);
  if (status != resources::FontAssetStatus::Ok ||
      size > resources::FontAssetService::kMaximumFontBytes)
    return failed(context, service->lastError(),
                  status == resources::FontAssetStatus::Unavailable
                      ? "unavailable"
                      : status == resources::FontAssetStatus::LimitExceeded
                            ? "limit-exceeded"
                            : "io");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  size_t read = 0;
  status = service->readInto(static_cast<resources::FontRole>(role),
                             bytes.data(), bytes.size(), read);
  if (status != resources::FontAssetStatus::Ok || read != bytes.size())
    return failed(context, service->lastError());
  return newBytes(context, bytes.data(), bytes.size());
}

JSValue jsAssetOpen(JSContext *context, JSValueConst, int argc,
                    JSValueConst *argv) {
  ApplicationContext *application = applicationFor(context);
  auto *service = application ? application->assets() : nullptr;
  std::string path;
  if (argc < 1 || !stringArgument(context, argv[0], path, 4096) ||
      !apps::validPackagePath(path) || path.back() == '/')
    return JS_ThrowTypeError(context, "invalid asset path");
  if (!service)
    return unavailable(context, application, 0, "package assets");
  uint32_t handle = 0;
  uint64_t size = 0;
  if (!service->open(path, handle, size))
    return failed(context, service->lastError());
  JSValue result = JS_NewObject(context);
  setU32(context, result, "handle", handle);
  setU64(context, result, "size", size);
  return result;
}

JSValue jsAssetRead(JSContext *context, JSValueConst, int argc,
                    JSValueConst *argv) {
  ApplicationContext *application = applicationFor(context);
  auto *service = application ? application->assets() : nullptr;
  uint32_t handle = 0;
  uint32_t maximum = 0;
  uint64_t offset = 0;
  if (argc < 3 || !uint32Argument(context, argv[0], handle) ||
      !uint64Argument(context, argv[1], offset) ||
      !uint32Argument(context, argv[2], maximum) ||
      maximum > resources::PackageAssetService::kMaximumReadBytes)
    return JS_ThrowTypeError(context, "invalid asset read arguments");
  if (!service)
    return unavailable(context, application, 0, "package assets");
  std::vector<uint8_t> bytes;
  if (!service->read(handle, offset, maximum, bytes))
    return failed(context, service->lastError());
  return newBytes(context, bytes.data(), bytes.size());
}

JSValue jsAssetClose(JSContext *context, JSValueConst, int argc,
                     JSValueConst *argv) {
  ApplicationContext *application = applicationFor(context);
  auto *service = application ? application->assets() : nullptr;
  uint32_t handle = 0;
  if (argc < 1 || !uint32Argument(context, argv[0], handle))
    return JS_ThrowTypeError(context, "asset close expects a handle");
  if (!service)
    return unavailable(context, application, 0, "package assets");
  if (!service->close(handle))
    return failed(context, service->lastError());
  return JS_UNDEFINED;
}

JSValue jsSystemRequest(JSContext *context, JSValueConst, int argc,
                        JSValueConst *argv) {
  ApplicationContext *application = applicationFor(context);
  const uint32_t permission =
      apps::permissionBit(apps::DeviceServicePermission::System);
  std::string service;
  std::string operation;
  std::string payload;
  if (argc < 3 || !stringArgument(context, argv[0], service, 64) ||
      !stringArgument(context, argv[1], operation, 64) ||
      !stringArgument(context, argv[2], payload, 256 * 1024))
    return JS_ThrowTypeError(context, "invalid system service request");
  auto *hub = application && application->permissionGranted(permission)
                  ? application->systemServices()
                  : nullptr;
  if (!hub)
    return unavailable(context, application, permission, "system services");
  std::string response;
  const int status = hub->request(application->appId(), {}, service, operation,
                                  payload, response, true);
  if (status == 0)
    return JS_NewStringLen(context, response.data(), response.size());
  const char *code = status == -EINVAL  ? "invalid-argument"
                     : status == -EACCES || status == -EPERM
                         ? "permission-denied"
                     : status == -ENOSYS || status == -ENOTSUP
                         ? "unavailable"
                     : status == -E2BIG      ? "limit-exceeded"
                     : status == -EBUSY      ? "busy"
                     : status == -ETIMEDOUT  ? "timeout"
                                             : "io";
  return failed(context, hub->lastError(), code);
}

JSValue audioStreamObject(JSContext *context,
                          const hardware::AudioStreamInfo &stream) {
  JSValue result = JS_NewObject(context);
  setI32(context, result, "sampleRate", stream.sample_rate);
  setI32(context, result, "channelCount", stream.channel_count);
  setI32(context, result, "deviceId", stream.device_id);
  setI64(context, result, "framesTransferred", stream.frames_transferred);
  return result;
}

JSValue jsAudioSupportedFormats(JSContext *context, JSValueConst, int,
                                JSValueConst *) {
  JSValue result = JS_NewArray(context);
  const auto &formats = media::supportedAudioFormats();
  for (uint32_t index = 0; index < formats.size(); ++index) {
    JSValue item = JS_NewObject(context);
    setString(context, item, "mimeType", formats[index].mime_type);
    setString(context, item, "extensions", formats[index].extensions);
    setU32(context, item, "decoder",
           static_cast<uint32_t>(formats[index].decoder));
    setBool(context, item, "streaming", formats[index].streaming);
    setBool(context, item, "seekable", formats[index].seekable);
    JS_SetPropertyUint32(context, result, index, item);
  }
  return result;
}

JSValue jsPcmCapabilities(JSContext *context, JSValueConst, int,
                          JSValueConst *) {
  JSValue result = JS_NewObject(context);
  setU32(context, result, "minimumSampleRate", 8000);
  setU32(context, result, "maximumSampleRate", 48000);
  setU32(context, result, "supportedChannelMask", 0x3);
  setU32(context, result, "minimumCapacityFrames", 256);
  setU32(context, result, "maximumCapacityFrames", 65536);
  return result;
}

media::MediaService *mediaFor(JSContext *context, JSValue &failure) {
  ApplicationContext *application = applicationFor(context);
  media::MediaService *media = application ? application->media() : nullptr;
  if (!media)
    failure = unavailable(context, application, 0, "audio media service");
  return media;
}

JSValue jsMediaLimits(JSContext *context, JSValueConst, int, JSValueConst *) {
  JSValue failure = JS_UNDEFINED;
  media::MediaService *media = mediaFor(context, failure);
  if (!media)
    return failure;
  const media::MediaSourceLimits limits = media->sourceLimits();
  JSValue result = JS_NewObject(context);
  setU64(context, result, "maximumSourceBytes", limits.maximum_source_bytes);
  setU64(context, result, "maximumSessionBytes", limits.maximum_session_bytes);
  setU32(context, result, "maximumSources", limits.maximum_sources);
  setU32(context, result, "maximumPlayers", limits.maximum_players);
  return result;
}

JSValue jsPcmOpen(JSContext *context, JSValueConst, int argc,
                  JSValueConst *argv) {
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
  uint32_t capacity = 0;
  uint32_t usage = 0;
  if (argc < 4 || !uint32Argument(context, argv[0], sample_rate) ||
      !uint32Argument(context, argv[1], channels) ||
      !uint32Argument(context, argv[2], capacity) ||
      !uint32Argument(context, argv[3], usage) || usage > 4 ||
      sample_rate > std::numeric_limits<int>::max() ||
      channels > std::numeric_limits<int>::max() ||
      capacity > std::numeric_limits<int>::max())
    return JS_ThrowRangeError(context, "invalid PCM configuration");
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, 0, "audio playback", failure);
  if (!service)
    return failure;
  hardware::PcmOutputConfig config = {
      static_cast<int>(sample_rate), static_cast<int>(channels),
      static_cast<int>(capacity), static_cast<hardware::AudioUsage>(usage)};
  uint32_t handle = 0;
  hardware::AudioStreamInfo info;
  if (!service->pcmOpen(config, handle, info))
    return failed(context, service->lastError());
  JSValue result = JS_NewObject(context);
  setU32(context, result, "handle", handle);
  JS_SetPropertyStr(context, result, "audioStream",
                    audioStreamObject(context, info));
  return result;
}

JSValue jsPcmWrite(JSContext *context, JSValueConst, int argc,
                   JSValueConst *argv) {
  uint32_t handle = 0;
  if (argc < 2 || !uint32Argument(context, argv[0], handle))
    return JS_ThrowTypeError(context, "pcmWrite expects a handle and Int16Array");
  size_t byte_offset = 0;
  size_t byte_length = 0;
  size_t bytes_per_element = 0;
  JSValue backing = JS_GetTypedArrayBuffer(
      context, argv[1], &byte_offset, &byte_length, &bytes_per_element);
  size_t backing_size = 0;
  uint8_t *bytes = JS_IsException(backing)
                       ? nullptr
                       : JS_GetArrayBuffer(context, &backing_size, backing);
  const bool valid = bytes && bytes_per_element == sizeof(int16_t) &&
                     byte_offset <= backing_size &&
                     byte_length <= backing_size - byte_offset &&
                     byte_length % sizeof(int16_t) == 0 &&
                     byte_length <= 4 * 1024 * 1024;
  JSValue failure = JS_UNDEFINED;
  auto *service = valid ? serviceFor(context, 0, "audio playback", failure)
                        : nullptr;
  hardware::PcmOutputStatus status;
  int64_t accepted = 0;
  const size_t sample_count = byte_length / sizeof(int16_t);
  const bool success =
      valid && service && service->pcmStatus(handle, status) &&
      status.stream.channel_count > 0 &&
      sample_count % static_cast<size_t>(status.stream.channel_count) == 0 &&
      service->pcmWrite(
          handle, reinterpret_cast<int16_t *>(bytes + byte_offset),
          sample_count / static_cast<size_t>(status.stream.channel_count),
          accepted);
  JS_FreeValue(context, backing);
  if (!valid)
    return JS_ThrowTypeError(context, "pcmWrite expects a bounded Int16Array");
  if (!service)
    return failure;
  if (!success)
    return failed(context, service->lastError());
  return JS_NewBigUint64(context,
                         static_cast<uint64_t>(std::max<int64_t>(0, accepted)));
}

enum class AudioHandleOperation {
  PcmSetVolume,
  PcmPause,
  PcmResume,
  PcmFlush,
  PcmStatus,
  PcmClose,
  SourceClose,
  PlayerPlay,
  PlayerPause,
  PlayerSeek,
  PlayerSetVolume,
  PlayerSetLooping,
  PlayerStatus,
  PlayerClose,
};

JSValue jsAudioHandle(JSContext *context, JSValueConst, int argc,
                      JSValueConst *argv, int magic) {
  uint32_t handle = 0;
  if (argc < 1 || !uint32Argument(context, argv[0], handle))
    return JS_ThrowTypeError(context, "audio operation expects a handle");
  const auto operation = static_cast<AudioHandleOperation>(magic);
  JSValue failure = JS_UNDEFINED;
  if (operation <= AudioHandleOperation::PcmClose) {
    auto *service = serviceFor(context, 0, "audio playback", failure);
    if (!service)
      return failure;
    if (operation == AudioHandleOperation::PcmStatus) {
      hardware::PcmOutputStatus status;
      if (!service->pcmStatus(handle, status))
        return failed(context, service->lastError());
      JSValue result = JS_NewObject(context);
      JS_SetPropertyStr(context, result, "audioStream",
                        audioStreamObject(context, status.stream));
      setI64(context, result, "queuedFrames", status.queued_frames);
      setI64(context, result, "consumedFrames", status.consumed_frames);
      setI32(context, result, "underruns", status.underruns);
      setBool(context, result, "paused", status.paused);
      return result;
    }
    bool success = false;
    if (operation == AudioHandleOperation::PcmSetVolume) {
      double volume = 0;
      if (argc < 2 || !doubleArgument(context, argv[1], volume) || volume < 0 ||
          volume > 1)
        return JS_ThrowRangeError(context, "volume must be between zero and one");
      success = service->pcmSetVolume(handle, static_cast<float>(volume));
    } else if (operation == AudioHandleOperation::PcmPause) {
      success = service->pcmPause(handle);
    } else if (operation == AudioHandleOperation::PcmResume) {
      success = service->pcmResume(handle);
    } else if (operation == AudioHandleOperation::PcmFlush) {
      success = service->pcmFlush(handle);
    } else {
      success = service->pcmClose(handle);
    }
    return success ? JS_UNDEFINED : failed(context, service->lastError());
  }

  media::MediaService *media = mediaFor(context, failure);
  if (!media)
    return failure;
  bool success = false;
  switch (operation) {
  case AudioHandleOperation::SourceClose:
    success = media->closeSource(handle);
    break;
  case AudioHandleOperation::PlayerPlay:
    success = media->play(handle);
    break;
  case AudioHandleOperation::PlayerPause:
    success = media->pause(handle);
    break;
  case AudioHandleOperation::PlayerSeek: {
    uint64_t position = 0;
    if (argc < 2 || !uint64Argument(context, argv[1], position))
      return JS_ThrowTypeError(context, "playerSeek expects a bigint position");
    success = media->seek(handle, position);
    break;
  }
  case AudioHandleOperation::PlayerSetVolume: {
    double volume = 0;
    if (argc < 2 || !doubleArgument(context, argv[1], volume) || volume < 0 ||
        volume > 1)
      return JS_ThrowRangeError(context, "volume must be between zero and one");
    success = media->setVolume(handle, static_cast<float>(volume));
    break;
  }
  case AudioHandleOperation::PlayerSetLooping: {
    bool looping = false;
    if (argc < 2 || !boolArgument(context, argv[1], looping))
      return JS_ThrowTypeError(context, "playerSetLooping expects a boolean");
    success = media->setLooping(handle, looping);
    break;
  }
  case AudioHandleOperation::PlayerStatus: {
    media::PlayerStatus status;
    if (!media->status(handle, status))
      return failed(context, media->lastError());
    JSValue result = JS_NewObject(context);
    setU32(context, result, "state", static_cast<uint32_t>(status.state));
    setU64(context, result, "positionMs", status.position_ms);
    setU64(context, result, "durationMs", status.duration_ms);
    setI32(context, result, "underruns", status.underruns);
    setU32(context, result, "failure", static_cast<uint32_t>(status.failure));
    return result;
  }
  case AudioHandleOperation::PlayerClose:
    success = media->close(handle);
    break;
  default:
    return JS_ThrowInternalError(context, "invalid audio operation");
  }
  return success ? JS_UNDEFINED : failed(context, media->lastError());
}

enum class AudioOpenOperation { Asset, SourceCreate, SourcePlayer };

JSValue jsAudioOpen(JSContext *context, JSValueConst, int argc,
                    JSValueConst *argv, int magic) {
  JSValue failure = JS_UNDEFINED;
  media::MediaService *media = mediaFor(context, failure);
  if (!media)
    return failure;
  uint32_t handle = 0;
  const auto operation = static_cast<AudioOpenOperation>(magic);
  if (operation == AudioOpenOperation::Asset) {
    std::string path;
    uint32_t usage = 0;
    if (argc < 2 || !stringArgument(context, argv[0], path, 1024) ||
        !uint32Argument(context, argv[1], usage) || usage > 4)
      return JS_ThrowTypeError(context, "playerOpenAsset expects path and usage");
    if (!media->openAsset(path, static_cast<hardware::AudioUsage>(usage),
                          handle))
      return failed(context, media->lastError());
  } else if (operation == AudioOpenOperation::SourceCreate) {
    const uint8_t *bytes = nullptr;
    size_t size = 0;
    JSValue backing = JS_UNDEFINED;
    std::string mime;
    std::string hint;
    const bool valid = argc >= 3 &&
                       bytesArgument(context, argv[0], bytes, size, backing,
                                     16 * 1024 * 1024) &&
                       size != 0 && stringArgument(context, argv[1], mime, 128) &&
                       stringArgument(context, argv[2], hint, 1024);
    const bool success = valid && media->createSource(bytes, size, mime, hint,
                                                       handle);
    JS_FreeValue(context, backing);
    if (!valid)
      return JS_ThrowTypeError(context, "sourceCreate expects bytes, MIME and locator");
    if (!success)
      return failed(context, media->lastError());
  } else {
    uint32_t source = 0;
    uint32_t usage = 0;
    if (argc < 2 || !uint32Argument(context, argv[0], source) ||
        !uint32Argument(context, argv[1], usage) || usage > 4)
      return JS_ThrowTypeError(context, "playerOpenSource expects source and usage");
    if (!media->openSource(source, static_cast<hardware::AudioUsage>(usage),
                           handle))
      return failed(context, media->lastError());
  }
  return JS_NewUint32(context, handle);
}

JSValue jsPlayTone(JSContext *context, JSValueConst, int argc,
                   JSValueConst *argv) {
  double frequency = 0;
  double volume = 0;
  uint32_t duration = 0;
  uint32_t usage = 0;
  if (argc < 4 || !doubleArgument(context, argv[0], frequency) ||
      !uint32Argument(context, argv[1], duration) ||
      !doubleArgument(context, argv[2], volume) ||
      !uint32Argument(context, argv[3], usage) || usage > 4 ||
      duration > kMaximumWaitMs || volume < 0 || volume > 1 || frequency <= 0)
    return JS_ThrowRangeError(context, "invalid tone arguments");
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, 0, "audio playback", failure);
  if (!service)
    return failure;
  hardware::AudioStreamInfo info;
  if (!service->playTone(frequency, static_cast<int>(duration),
                         static_cast<float>(volume),
                         static_cast<hardware::AudioUsage>(usage), info))
    return failed(context, service->lastError());
  return audioStreamObject(context, info);
}

JSValue jsRecordWav(JSContext *context, JSValueConst, int argc,
                    JSValueConst *argv) {
  std::string path;
  uint32_t duration = 0;
  if (argc < 2 || !stringArgument(context, argv[0], path, 4096) ||
      !uint32Argument(context, argv[1], duration) || duration > kMaximumWaitMs)
    return JS_ThrowTypeError(context, "recordWav expects path and bounded duration");
  const uint32_t permission =
      apps::permissionBit(apps::DeviceServicePermission::AudioCapture);
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, permission, "audio capture", failure);
  if (!service)
    return failure;
  hardware::RecordingResult recording;
  if (!service->recordWav(path, static_cast<int>(duration), recording))
    return failed(context, service->lastError());
  JSValue result = JS_NewObject(context);
  JS_SetPropertyStr(context, result, "audioStream",
                    audioStreamObject(context, recording.stream));
  JS_SetPropertyStr(context, result, "peak",
                    JS_NewFloat64(context, recording.peak));
  JS_SetPropertyStr(context, result, "rms",
                    JS_NewFloat64(context, recording.rms));
  setString(context, result, "path", recording.path);
  return result;
}

JSValue jsAudioLastError(JSContext *context, JSValueConst, int,
                         JSValueConst *) {
  ApplicationContext *application = applicationFor(context);
  media::MediaService *media = application ? application->activeMedia() : nullptr;
  if (media && !media->lastError().empty())
    return JS_NewString(context, media->lastError().c_str());
  device::ServiceProvider *service = application ? application->services() : nullptr;
  const std::string value = !service ? "service unavailable"
                            : service->lastError().empty()
                                ? "service ready"
                                : service->lastError();
  return JS_NewStringLen(context, value.data(), value.size());
}

JSValue jsServiceLastError(JSContext *context, JSValueConst, int,
                           JSValueConst *) {
  ApplicationContext *application = applicationFor(context);
  device::ServiceProvider *service = application ? application->services() : nullptr;
  const std::string value = !service ? "service unavailable"
                            : service->lastError().empty()
                                ? "service ready"
                                : service->lastError();
  return JS_NewStringLen(context, value.data(), value.size());
}

JSValue jsCameraEnumerate(JSContext *context, JSValueConst, int,
                          JSValueConst *) {
  const uint32_t permission =
      apps::permissionBit(apps::DeviceServicePermission::Camera);
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, permission, "camera", failure);
  if (!service)
    return failure;
  std::vector<hardware::CameraInfo> cameras;
  if (!service->enumerateCameras(cameras) || cameras.size() > 32)
    return failed(context, service->lastError());
  JSValue result = JS_NewArray(context);
  for (uint32_t index = 0; index < cameras.size(); ++index) {
    JSValue item = JS_NewObject(context);
    setString(context, item, "id", cameras[index].id);
    setU32(context, item, "facing",
           static_cast<uint32_t>(cameras[index].facing));
    setI32(context, item, "sensorOrientation",
           cameras[index].sensor_orientation);
    setI32(context, item, "hardwareLevel", cameras[index].hardware_level);
    setBool(context, item, "flashAvailable", cameras[index].flash_available);
    setI32(context, item, "maxJpegWidth", cameras[index].max_jpeg_width);
    setI32(context, item, "maxJpegHeight", cameras[index].max_jpeg_height);
    JS_SetPropertyUint32(context, result, index, item);
  }
  return result;
}

JSValue jsCameraTorch(JSContext *context, JSValueConst, int argc,
                      JSValueConst *argv) {
  std::string id;
  bool enabled = false;
  if (argc < 2 || !stringArgument(context, argv[0], id, 128) ||
      !boolArgument(context, argv[1], enabled))
    return JS_ThrowTypeError(context, "setTorch expects camera id and boolean");
  const uint32_t permission =
      apps::permissionBit(apps::DeviceServicePermission::Camera);
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, permission, "camera", failure);
  if (!service)
    return failure;
  return service->setTorch(id, enabled)
             ? JS_UNDEFINED
             : failed(context, service->lastError());
}

JSValue jsCameraCapture(JSContext *context, JSValueConst, int argc,
                        JSValueConst *argv) {
  std::string id;
  std::string path;
  int32_t width = 0;
  int32_t height = 0;
  bool flash = false;
  uint32_t timeout = 0;
  if (argc < 6 || !stringArgument(context, argv[0], id, 128) ||
      !stringArgument(context, argv[1], path, 4096) ||
      !int32Argument(context, argv[2], width) || width <= 0 ||
      !int32Argument(context, argv[3], height) || height <= 0 ||
      !boolArgument(context, argv[4], flash) ||
      !uint32Argument(context, argv[5], timeout) || timeout > kMaximumWaitMs)
    return JS_ThrowTypeError(context, "invalid JPEG capture arguments");
  const uint32_t permission =
      apps::permissionBit(apps::DeviceServicePermission::Camera);
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, permission, "camera", failure);
  if (!service)
    return failure;
  hardware::PhotoResult photo;
  if (!service->captureJpeg(id, path, photo, width, height, flash,
                            static_cast<int>(timeout)))
    return failed(context, service->lastError());
  JSValue result = JS_NewObject(context);
  setString(context, result, "path", photo.path);
  setI32(context, result, "width", photo.width);
  setI32(context, result, "height", photo.height);
  setU64(context, result, "bytes", photo.byte_count);
  return result;
}

JSValue batteryObject(JSContext *context,
                      const hardware::BatterySnapshot &snapshot) {
  JSValue result = JS_NewObject(context);
  setU32(context, result, "state", static_cast<uint32_t>(snapshot.state));
  setI32(context, result, "capacityPercent", snapshot.capacity_percent);
  setI32(context, result, "voltageMicrovolts", snapshot.voltage_microvolts);
  setI32(context, result, "currentMicroamps", snapshot.current_microamps);
  setI32(context, result, "temperatureTenthsCelsius",
         snapshot.temperature_tenths_celsius);
  setBool(context, result, "usbOnline", snapshot.usb_online);
  return result;
}

JSValue jsPowerBattery(JSContext *context, JSValueConst, int argc,
                       JSValueConst *argv, int magic) {
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, 0, "battery", failure);
  if (!service)
    return failure;
  hardware::BatterySnapshot snapshot;
  if (magic == 0) {
    if (!service->queryBattery(snapshot))
      return failed(context, service->lastError());
    return batteryObject(context, snapshot);
  }
  uint32_t timeout = 0;
  if (argc < 1 || !uint32Argument(context, argv[0], timeout) ||
      timeout > kMaximumWaitMs)
    return JS_ThrowRangeError(context, "invalid battery event timeout");
  const int changed =
      service->waitForBatteryEvent(static_cast<int>(timeout), snapshot);
  if (changed < 0)
    return failed(context, service->lastError());
  return changed == 0 ? JS_NULL : batteryObject(context, snapshot);
}

enum class PowerOperation {
  SetInteractive,
  AcquireWakeLock,
  ReleaseWakeLock,
  EnableAutoSuspend,
  DisableAutoSuspend,
  ScheduleRtcWake,
  ClearRtcWake,
  Suspend,
  QueryFlipState,
};

JSValue jsPowerOperation(JSContext *context, JSValueConst, int argc,
                         JSValueConst *argv, int magic) {
  const uint32_t permission =
      apps::permissionBit(apps::DeviceServicePermission::Power);
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, permission, "power", failure);
  if (!service)
    return failure;
  ApplicationContext *application = applicationFor(context);
  const auto operation = static_cast<PowerOperation>(magic);
  bool success = false;
  switch (operation) {
  case PowerOperation::SetInteractive: {
    bool enabled = false;
    if (argc < 1 || !boolArgument(context, argv[0], enabled))
      return JS_ThrowTypeError(context, "setInteractive expects a boolean");
    success = service->setInteractive(enabled);
    break;
  }
  case PowerOperation::AcquireWakeLock:
  case PowerOperation::ReleaseWakeLock: {
    std::string name;
    if (argc < 1 || !stringArgument(context, argv[0], name, 128) || name.empty())
      return JS_ThrowTypeError(context, "wake lock expects a bounded name");
    success = operation == PowerOperation::AcquireWakeLock
                  ? application->acquireWakeLock(name)
                  : application->releaseWakeLock(name);
    break;
  }
  case PowerOperation::EnableAutoSuspend:
    success = service->enableAutoSuspend();
    break;
  case PowerOperation::DisableAutoSuspend:
    success = service->disableAutoSuspend();
    break;
  case PowerOperation::ScheduleRtcWake: {
    uint32_t delay = 0;
    if (argc < 1 || !uint32Argument(context, argv[0], delay) ||
        delay > static_cast<uint32_t>(std::numeric_limits<int>::max()))
      return JS_ThrowRangeError(context, "invalid RTC wake delay");
    success = service->scheduleRtcWake(static_cast<int>(delay));
    break;
  }
  case PowerOperation::ClearRtcWake:
    success = service->clearRtcWake();
    break;
  case PowerOperation::Suspend: {
    uint32_t timeout = 0;
    if (argc < 1 || !uint32Argument(context, argv[0], timeout) ||
        timeout > kMaximumWaitMs)
      return JS_ThrowRangeError(context, "invalid suspend timeout");
    success = service->suspend(static_cast<int>(timeout));
    break;
  }
  case PowerOperation::QueryFlipState:
    return JS_NewUint32(context,
                        static_cast<uint32_t>(service->queryFlipState()));
  }
  return success ? JS_UNDEFINED : failed(context, service->lastError());
}

enum class VibratorOperation { Vibrate, Stop, SupportsAmplitude, SetAmplitude };

JSValue jsVibrator(JSContext *context, JSValueConst, int argc,
                   JSValueConst *argv, int magic) {
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, 0, "vibrator", failure);
  if (!service)
    return failure;
  const auto operation = static_cast<VibratorOperation>(magic);
  if (operation == VibratorOperation::SupportsAmplitude)
    return JS_NewBool(context, service->supportsAmplitudeControl());
  bool success = false;
  if (operation == VibratorOperation::Vibrate) {
    uint32_t duration = 0;
    if (argc < 1 || !uint32Argument(context, argv[0], duration) ||
        duration > kMaximumWaitMs)
      return JS_ThrowRangeError(context, "invalid vibration duration");
    success = service->vibrate(duration);
  } else if (operation == VibratorOperation::Stop) {
    success = service->stopVibration();
  } else {
    uint32_t amplitude = 0;
    if (argc < 1 || !uint32Argument(context, argv[0], amplitude) ||
        amplitude > 255)
      return JS_ThrowRangeError(context, "vibration amplitude exceeds 255");
    success = service->setVibrationAmplitude(static_cast<uint8_t>(amplitude));
  }
  return success ? JS_UNDEFINED : failed(context, service->lastError());
}

JSValue wifiStatusObject(JSContext *context,
                         const network::WifiStatus &status) {
  JSValue result = JS_NewObject(context);
  setString(context, result, "state", status.state);
  setString(context, result, "ssid", status.ssid);
  setString(context, result, "bssid", status.bssid);
  setString(context, result, "ipAddress", status.ip_address);
  setI32(context, result, "networkId", status.network_id);
  return result;
}

enum class WifiQuery { Status, Scan, Networks };

JSValue jsWifiQuery(JSContext *context, JSValueConst, int argc,
                    JSValueConst *argv, int magic) {
  const uint32_t permission =
      apps::permissionBit(apps::DeviceServicePermission::Wifi);
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, permission, "wifi", failure);
  if (!service)
    return failure;
  const auto operation = static_cast<WifiQuery>(magic);
  if (operation == WifiQuery::Status) {
    network::WifiStatus status;
    return service->wifiStatus(status) ? wifiStatusObject(context, status)
                                       : failed(context, service->lastError());
  }
  JSValue result = JS_NewArray(context);
  if (operation == WifiQuery::Scan) {
    uint32_t wait = 0;
    if (argc < 1 || !uint32Argument(context, argv[0], wait) ||
        wait > kMaximumWaitMs)
      return JS_ThrowRangeError(context, "invalid wifi scan timeout");
    std::vector<network::WifiAccessPoint> access_points;
    if (!service->wifiScan(access_points, static_cast<int>(wait)) ||
        access_points.size() > 256)
      return failed(context, service->lastError());
    for (uint32_t index = 0; index < access_points.size(); ++index) {
      JSValue item = JS_NewObject(context);
      setString(context, item, "bssid", access_points[index].bssid);
      setI32(context, item, "frequencyMhz", access_points[index].frequency_mhz);
      setI32(context, item, "signalDbm", access_points[index].signal_dbm);
      setString(context, item, "capabilities", access_points[index].flags);
      setString(context, item, "ssid", access_points[index].ssid);
      JS_SetPropertyUint32(context, result, index, item);
    }
  } else {
    std::vector<network::WifiNetwork> networks;
    if (!service->wifiListNetworks(networks) || networks.size() > 256)
      return failed(context, service->lastError());
    for (uint32_t index = 0; index < networks.size(); ++index) {
      JSValue item = JS_NewObject(context);
      setI32(context, item, "id", networks[index].id);
      setString(context, item, "ssid", networks[index].ssid);
      setString(context, item, "bssid", networks[index].bssid);
      setString(context, item, "capabilities", networks[index].flags);
      JS_SetPropertyUint32(context, result, index, item);
    }
  }
  return result;
}

enum class WifiAction { Connect, Disconnect, Reconnect, Forget, Save };

JSValue jsWifiAction(JSContext *context, JSValueConst, int argc,
                     JSValueConst *argv, int magic) {
  const uint32_t permission =
      apps::permissionBit(apps::DeviceServicePermission::Wifi);
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, permission, "wifi", failure);
  if (!service)
    return failure;
  const auto operation = static_cast<WifiAction>(magic);
  bool success = false;
  if (operation == WifiAction::Connect) {
    std::string ssid;
    std::string credential;
    uint32_t security = 0;
    if (argc < 3 || !stringArgument(context, argv[0], ssid, 32) ||
        !uint32Argument(context, argv[1], security) || security > 1 ||
        !stringArgument(context, argv[2], credential, 64))
      return JS_ThrowTypeError(context, "invalid wifi connection arguments");
    int network = -1;
    if (!service->wifiConnect(ssid,
                              static_cast<network::WifiSecurity>(security),
                              credential, network))
      return failed(context, service->lastError());
    return JS_NewInt32(context, network);
  }
  if (operation == WifiAction::Disconnect)
    success = service->wifiDisconnect();
  else if (operation == WifiAction::Reconnect)
    success = service->wifiReconnect();
  else if (operation == WifiAction::Save)
    success = service->wifiSaveConfiguration();
  else {
    int32_t network = -1;
    if (argc < 1 || !int32Argument(context, argv[0], network))
      return JS_ThrowTypeError(context, "wifi forget expects a network id");
    success = service->wifiForget(network);
  }
  return success ? JS_UNDEFINED : failed(context, service->lastError());
}

JSValue ipConfigurationObject(JSContext *context,
                              const network::IpConfiguration &configuration) {
  JSValue result = JS_NewObject(context);
  setString(context, result, "interfaceName", configuration.interface_name);
  setString(context, result, "address", configuration.address);
  setU32(context, result, "prefixLength", configuration.prefix_length);
  setString(context, result, "gateway", configuration.gateway);
  setString(context, result, "dns1", configuration.dns1);
  setString(context, result, "dns2", configuration.dns2);
  return result;
}

JSValue jsIpStatus(JSContext *context, JSValueConst, int, JSValueConst *) {
  const uint32_t permission =
      apps::permissionBit(apps::DeviceServicePermission::Wifi);
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, permission, "IP configuration", failure);
  if (!service)
    return failure;
  network::IpConfiguration configuration;
  return service->ipStatus(configuration)
             ? ipConfigurationObject(context, configuration)
             : failed(context, service->lastError());
}

JSValue jsIpDhcp(JSContext *context, JSValueConst, int argc,
                 JSValueConst *argv) {
  uint32_t timeout = 0;
  if (argc < 1 || !uint32Argument(context, argv[0], timeout) ||
      timeout > kMaximumWaitMs)
    return JS_ThrowRangeError(context, "invalid DHCP timeout");
  const uint32_t permission =
      apps::permissionBit(apps::DeviceServicePermission::Wifi);
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, permission, "IP configuration", failure);
  if (!service)
    return failure;
  return service->ipUseDhcp(static_cast<int>(timeout))
             ? JS_UNDEFINED
             : failed(context, service->lastError());
}

JSValue jsIpStatic(JSContext *context, JSValueConst, int argc,
                   JSValueConst *argv) {
  if (argc < 1 || !JS_IsObject(argv[0]))
    return JS_ThrowTypeError(context, "useStatic expects a configuration object");
  network::IpConfiguration configuration;
  uint32_t prefix = 0;
  if (!getStringProperty(context, argv[0], "interfaceName",
                         configuration.interface_name, 64) ||
      !getStringProperty(context, argv[0], "address", configuration.address,
                         64) ||
      !getU32Property(context, argv[0], "prefixLength", prefix) ||
      !getStringProperty(context, argv[0], "gateway", configuration.gateway,
                         64) ||
      !getStringProperty(context, argv[0], "dns1", configuration.dns1, 64) ||
      !getStringProperty(context, argv[0], "dns2", configuration.dns2, 64))
    return JS_ThrowTypeError(context, "invalid static IP configuration");
  configuration.prefix_length = prefix;
  const uint32_t permission =
      apps::permissionBit(apps::DeviceServicePermission::Wifi);
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, permission, "IP configuration", failure);
  if (!service)
    return failure;
  return service->ipUseStatic(configuration)
             ? JS_UNDEFINED
             : failed(context, service->lastError());
}

enum class BluetoothOperation {
  Enable,
  Disable,
  ClassicScan,
  LeScan,
  Pair,
  Unpair,
  CancelPairing,
  ProfileConnect,
  ProfileDisconnect,
  ProfileCycle,
  LeCycle,
};

network::BluetoothProfile bluetoothProfile(uint32_t value) {
  return static_cast<network::BluetoothProfile>(value == 0   ? 0x03
                                                   : value == 1 ? 0x05
                                                                : 0x06);
}

JSValue jsBluetooth(JSContext *context, JSValueConst, int argc,
                    JSValueConst *argv, int magic) {
  const uint32_t permission =
      apps::permissionBit(apps::DeviceServicePermission::Bluetooth);
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, permission, "bluetooth", failure);
  if (!service)
    return failure;
  const auto operation = static_cast<BluetoothOperation>(magic);
  if (operation == BluetoothOperation::Enable ||
      operation == BluetoothOperation::Disable) {
    uint32_t timeout = 0;
    if (argc < 1 || !uint32Argument(context, argv[0], timeout) ||
        timeout > kMaximumWaitMs)
      return JS_ThrowRangeError(context, "invalid bluetooth timeout");
    const bool success = operation == BluetoothOperation::Enable
                             ? service->bluetoothEnable(timeout)
                             : service->bluetoothDisable(timeout);
    return success ? JS_UNDEFINED : failed(context, service->lastError());
  }
  if (operation == BluetoothOperation::ClassicScan ||
      operation == BluetoothOperation::LeScan) {
    uint32_t duration = 0;
    if (argc < 1 || !uint32Argument(context, argv[0], duration) ||
        duration > kMaximumWaitMs)
      return JS_ThrowRangeError(context, "invalid bluetooth scan duration");
    std::vector<network::BluetoothDevice> devices;
    const bool success = operation == BluetoothOperation::ClassicScan
                             ? service->bluetoothClassicScan(devices, duration)
                             : service->bluetoothLeScan(devices, duration);
    if (!success || devices.size() > 256)
      return failed(context, service->lastError());
    JSValue result = JS_NewArray(context);
    for (uint32_t index = 0; index < devices.size(); ++index) {
      JSValue item = JS_NewObject(context);
      setString(context, item, "address", devices[index].address);
      setString(context, item, "name", devices[index].name);
      setI32(context, item, "rssi", devices[index].rssi);
      setU32(context, item, "deviceClass", devices[index].device_class);
      setI32(context, item, "deviceType", devices[index].device_type);
      JS_SetPropertyStr(
          context, item, "advertisingData",
          newBytes(context, devices[index].advertising_data.data(),
                   devices[index].advertising_data.size()));
      JS_SetPropertyUint32(context, result, index, item);
    }
    return result;
  }

  std::string address;
  if (argc < 1 || !stringArgument(context, argv[0], address, 32))
    return JS_ThrowTypeError(context, "bluetooth operation expects an address");
  bool success = false;
  if (operation == BluetoothOperation::Pair) {
    uint32_t transport = 0;
    if (argc < 2 || !uint32Argument(context, argv[1], transport) ||
        transport > 2)
      return JS_ThrowRangeError(context, "invalid bluetooth transport");
    success = service->bluetoothPair(
        address, static_cast<network::BluetoothTransport>(transport));
  } else if (operation == BluetoothOperation::Unpair) {
    success = service->bluetoothUnpair(address);
  } else if (operation == BluetoothOperation::CancelPairing) {
    success = service->bluetoothCancelPairing(address);
  } else if (operation == BluetoothOperation::LeCycle) {
    uint32_t hold = 0;
    uint32_t timeout = 0;
    if (argc < 3 || !uint32Argument(context, argv[1], hold) ||
        !uint32Argument(context, argv[2], timeout) || hold > kMaximumWaitMs ||
        timeout > kMaximumWaitMs)
      return JS_ThrowRangeError(context, "invalid bluetooth connection timing");
    success = service->bluetoothLeConnectionCycle(address, hold, timeout);
  } else {
    uint32_t profile = 0;
    if (argc < 2 || !uint32Argument(context, argv[1], profile) || profile > 2)
      return JS_ThrowRangeError(context, "invalid bluetooth profile");
    if (operation == BluetoothOperation::ProfileConnect) {
      success = service->bluetoothProfileConnect(address,
                                                  bluetoothProfile(profile));
    } else if (operation == BluetoothOperation::ProfileDisconnect) {
      success = service->bluetoothProfileDisconnect(
          address, bluetoothProfile(profile));
    } else {
      uint32_t hold = 0;
      if (argc < 3 || !uint32Argument(context, argv[2], hold) ||
          hold > kMaximumWaitMs)
        return JS_ThrowRangeError(context, "invalid bluetooth profile hold time");
      success = service->bluetoothProfileConnectionCycle(
          address, bluetoothProfile(profile), hold);
    }
  }
  return success ? JS_UNDEFINED : failed(context, service->lastError());
}

JSValue modemRequestObject(JSContext *context,
                           const modem::ModemRequestStatus &status) {
  JSValue result = JS_NewObject(context);
  setString(context, result, "operation", status.operation);
  setI32(context, result, "error", status.error);
  setBool(context, result, "timedOut", status.timed_out);
  return result;
}

JSValue registrationObject(JSContext *context,
                           const modem::RegistrationStatus &status) {
  JSValue result = JS_NewObject(context);
  setI32(context, result, "state", status.state);
  setI32(context, result, "radioTechnology", status.radio_technology);
  setI32(context, result, "denialReason", status.denial_reason);
  setI32(context, result, "maxDataCalls", status.max_data_calls);
  return result;
}

JSValue jsModemSnapshot(JSContext *context, JSValueConst, int argc,
                        JSValueConst *argv) {
  uint32_t timeout = 0;
  if (argc < 1 || !uint32Argument(context, argv[0], timeout) ||
      timeout > kMaximumWaitMs)
    return JS_ThrowRangeError(context, "invalid modem timeout");
  const uint32_t modem_permission =
      apps::permissionBit(apps::DeviceServicePermission::Modem);
  const uint32_t identity_permission =
      apps::permissionBit(apps::DeviceServicePermission::ModemIdentity);
  ApplicationContext *application = applicationFor(context);
  if (!application || !application->permissionGranted(identity_permission))
    return unavailable(context, application, identity_permission,
                       "modem identity");
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, modem_permission, "modem", failure);
  if (!service)
    return failure;
  modem::ModemSnapshot snapshot;
  if (!service->modemSnapshot(snapshot, timeout) || snapshot.requests.size() > 256)
    return failed(context, service->lastError());
  JSValue result = JS_NewObject(context);
  setBool(context, result, "serviceConnected", snapshot.service_connected);
  setI32(context, result, "radioState", snapshot.radio_state);
  setString(context, result, "basebandVersion", snapshot.baseband_version);
  JSValue identity = JS_NewObject(context);
  setString(context, identity, "imei", snapshot.identity.imei);
  setString(context, identity, "imeiSoftwareVersion",
            snapshot.identity.imei_software_version);
  setString(context, identity, "esn", snapshot.identity.esn);
  setString(context, identity, "meid", snapshot.identity.meid);
  JS_SetPropertyStr(context, result, "identity", identity);
  JSValue sim = JS_NewObject(context);
  setI32(context, sim, "cardState", snapshot.sim.card_state);
  setI32(context, sim, "universalPinState", snapshot.sim.universal_pin_state);
  setI32(context, sim, "applicationCount", snapshot.sim.application_count);
  JS_SetPropertyStr(context, result, "sim", sim);
  JSValue signal = JS_NewObject(context);
  const struct SignalField {
    const char *name;
    int value;
  } signal_fields[] = {
      {"gsmStrength", snapshot.signal.gsm_strength},
      {"gsmBitErrorRate", snapshot.signal.gsm_bit_error_rate},
      {"cdmaDbm", snapshot.signal.cdma_dbm},
      {"cdmaEcio", snapshot.signal.cdma_ecio},
      {"evdoDbm", snapshot.signal.evdo_dbm},
      {"evdoEcio", snapshot.signal.evdo_ecio},
      {"evdoSnr", snapshot.signal.evdo_snr},
      {"lteStrength", snapshot.signal.lte_strength},
      {"lteRsrp", snapshot.signal.lte_rsrp},
      {"lteRsrq", snapshot.signal.lte_rsrq},
      {"lteRssnr", snapshot.signal.lte_rssnr},
      {"lteCqi", snapshot.signal.lte_cqi},
      {"lteTimingAdvance", snapshot.signal.lte_timing_advance},
      {"tdscdmaRscp", snapshot.signal.tdscdma_rscp},
  };
  for (const SignalField &field : signal_fields)
    setI32(context, signal, field.name, field.value);
  JS_SetPropertyStr(context, result, "signal", signal);
  JS_SetPropertyStr(context, result, "voiceRegistration",
                    registrationObject(context, snapshot.voice_registration));
  JS_SetPropertyStr(context, result, "dataRegistration",
                    registrationObject(context, snapshot.data_registration));
  JSValue network_operator = JS_NewObject(context);
  setString(context, network_operator, "longName",
            snapshot.network_operator.long_name);
  setString(context, network_operator, "shortName",
            snapshot.network_operator.short_name);
  setString(context, network_operator, "numeric",
            snapshot.network_operator.numeric);
  JS_SetPropertyStr(context, result, "networkOperator", network_operator);
  setI32(context, result, "preferredNetworkType",
         snapshot.preferred_network_type);
  setI32(context, result, "voiceRadioTechnology",
         snapshot.voice_radio_technology);
  setI32(context, result, "currentCallCount", snapshot.current_call_count);
  setI32(context, result, "dataCallCount", snapshot.data_call_count);
  setI32(context, result, "hardwareConfigCount", snapshot.hardware_config_count);
  setU32(context, result, "radioAccessFamily", snapshot.radio_access_family);
  setString(context, result, "logicalModemUuid", snapshot.logical_modem_uuid);
  JSValue requests = JS_NewArray(context);
  for (uint32_t index = 0; index < snapshot.requests.size(); ++index)
    JS_SetPropertyUint32(context, requests, index,
                         modemRequestObject(context, snapshot.requests[index]));
  JS_SetPropertyStr(context, result, "requests", requests);
  return result;
}

JSValue jsModemRadioPower(JSContext *context, JSValueConst, int argc,
                          JSValueConst *argv) {
  bool enabled = false;
  uint32_t timeout = 0;
  if (argc < 2 || !boolArgument(context, argv[0], enabled) ||
      !uint32Argument(context, argv[1], timeout) || timeout > kMaximumWaitMs)
    return JS_ThrowTypeError(context, "invalid radio power arguments");
  const uint32_t modem_permission =
      apps::permissionBit(apps::DeviceServicePermission::Modem);
  const uint32_t control_permission =
      apps::permissionBit(apps::DeviceServicePermission::ModemRadioControl);
  ApplicationContext *application = applicationFor(context);
  if (!application || !application->permissionGranted(control_permission))
    return unavailable(context, application, control_permission,
                       "modem radio control");
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, modem_permission, "modem", failure);
  if (!service)
    return failure;
  modem::ModemRequestStatus status;
  return service->setRadioPower(enabled, status, timeout)
             ? modemRequestObject(context, status)
             : failed(context, service->lastError());
}

JSValue jsCodecRoundTrip(JSContext *context, JSValueConst, int argc,
                         JSValueConst *argv) {
  int32_t width = 0;
  int32_t height = 0;
  int32_t frames = 0;
  uint32_t timeout = 0;
  if (argc < 4 || !int32Argument(context, argv[0], width) || width <= 0 ||
      !int32Argument(context, argv[1], height) || height <= 0 ||
      !int32Argument(context, argv[2], frames) || frames <= 0 ||
      !uint32Argument(context, argv[3], timeout) || timeout > kMaximumWaitMs)
    return JS_ThrowRangeError(context, "invalid codec round-trip arguments");
  JSValue failure = JS_UNDEFINED;
  auto *service = serviceFor(context, 0, "hardware codec", failure);
  if (!service)
    return failure;
  hardware::CodecResult codec;
  if (!service->testH264RoundTrip(width, height, frames, codec, timeout))
    return failed(context, service->lastError());
  JSValue result = JS_NewObject(context);
  setString(context, result, "encoderName", codec.encoder_name);
  setString(context, result, "decoderName", codec.decoder_name);
  setBool(context, result, "encoderHardwareAccelerated",
          codec.encoder_hardware_accelerated);
  setBool(context, result, "decoderHardwareAccelerated",
          codec.decoder_hardware_accelerated);
  setI32(context, result, "width", codec.width);
  setI32(context, result, "height", codec.height);
  setI32(context, result, "inputFrames", codec.input_frames);
  setI32(context, result, "outputBuffers", codec.output_buffers);
  setI32(context, result, "decodedFrames", codec.decoded_frames);
  setU64(context, result, "encodedBytes", codec.encoded_bytes);
  return result;
}

struct Export {
  const char *name;
  JSCFunction *function = nullptr;
  JSCFunctionMagic *magic_function = nullptr;
  int arguments;
  int magic;
  bool uses_magic;

  constexpr Export(const char *next_name, JSCFunction *next_function,
                   int next_arguments, int next_magic, bool next_uses_magic)
      : name(next_name), function(next_function), arguments(next_arguments),
        magic(next_magic), uses_magic(next_uses_magic) {}

  constexpr Export(const char *next_name,
                   JSCFunctionMagic *next_function, int next_arguments,
                   int next_magic, bool next_uses_magic)
      : name(next_name), magic_function(next_function),
        arguments(next_arguments), magic(next_magic),
        uses_magic(next_uses_magic) {}
};

constexpr Export kDeviceExports[] = {
    {"getDescriptor", jsDeviceDescriptor, 0, 0, false},
    {"getCapability", jsDeviceCapability, 1, 0, false},
    {"getAccess", jsDeviceAccess, 1, 0, false},
};

constexpr Export kStorageExports[] = {
    {"kvGet", jsKvGet, 1, 0, false},
    {"kvSet", jsKvSet, 2, 0, false},
    {"kvDelete", jsKvDelete, 1, 0, false},
    {"kvClear", jsKvClear, 0, 0, false},
    {"databaseExecute", jsDatabase, 2,
     static_cast<int>(DatabaseOperation::Execute), true},
    {"databasePrepare", jsDatabase, 2,
     static_cast<int>(DatabaseOperation::Prepare), true},
    {"statementBindNull", jsDatabase, 2,
     static_cast<int>(DatabaseOperation::BindNull), true},
    {"statementBindInteger", jsDatabase, 3,
     static_cast<int>(DatabaseOperation::BindInteger), true},
    {"statementBindFloat", jsDatabase, 3,
     static_cast<int>(DatabaseOperation::BindFloat), true},
    {"statementBindText", jsDatabase, 3,
     static_cast<int>(DatabaseOperation::BindText), true},
    {"statementBindBlob", jsDatabase, 3,
     static_cast<int>(DatabaseOperation::BindBlob), true},
    {"statementStep", jsDatabase, 1,
     static_cast<int>(DatabaseOperation::Step), true},
    {"statementColumnCount", jsDatabase, 1,
     static_cast<int>(DatabaseOperation::ColumnCount), true},
    {"statementColumnKind", jsDatabase, 2,
     static_cast<int>(DatabaseOperation::ColumnKind), true},
    {"statementColumnInteger", jsDatabase, 2,
     static_cast<int>(DatabaseOperation::ColumnInteger), true},
    {"statementColumnFloat", jsDatabase, 2,
     static_cast<int>(DatabaseOperation::ColumnFloat), true},
    {"statementColumnText", jsDatabase, 2,
     static_cast<int>(DatabaseOperation::ColumnText), true},
    {"statementColumnBlob", jsDatabase, 2,
     static_cast<int>(DatabaseOperation::ColumnBlob), true},
    {"statementFinish", jsDatabase, 1,
     static_cast<int>(DatabaseOperation::Finish), true},
};

constexpr Export kDeviceStorageExports[] = {
    {"enumerateFiles", jsDeviceStorageList, 1, 0, false},
    {"readFile", jsDeviceStorageRead, 2, 0, false},
    {"writeFile", jsDeviceStorageWrite, 4, 0, false},
    {"deleteFile", jsDeviceStorageDelete, 2, 0, false},
    {"freeSpace", jsDeviceStorageSpace, 1, 0, true},
    {"usedSpace", jsDeviceStorageSpace, 1, 1, true},
};

constexpr Export kFontExports[] = {{"load", jsFontLoad, 1, 0, false}};
constexpr Export kAssetExports[] = {
    {"open", jsAssetOpen, 1, 0, false},
    {"read", jsAssetRead, 3, 0, false},
    {"close", jsAssetClose, 1, 0, false},
};
constexpr Export kSystemExports[] = {
    {"request", jsSystemRequest, 3, 0, false},
};

constexpr Export kAudioExports[] = {
    {"supportedFormats", jsAudioSupportedFormats, 0, 0, false},
    {"getPcmCapabilities", jsPcmCapabilities, 0, 0, false},
    {"getSourceLimits", jsMediaLimits, 0, 0, false},
    {"pcmOpen", jsPcmOpen, 4, 0, false},
    {"pcmWrite", jsPcmWrite, 2, 0, false},
    {"pcmSetVolume", jsAudioHandle, 2,
     static_cast<int>(AudioHandleOperation::PcmSetVolume), true},
    {"pcmPause", jsAudioHandle, 1,
     static_cast<int>(AudioHandleOperation::PcmPause), true},
    {"pcmResume", jsAudioHandle, 1,
     static_cast<int>(AudioHandleOperation::PcmResume), true},
    {"pcmFlush", jsAudioHandle, 1,
     static_cast<int>(AudioHandleOperation::PcmFlush), true},
    {"pcmStatus", jsAudioHandle, 1,
     static_cast<int>(AudioHandleOperation::PcmStatus), true},
    {"pcmClose", jsAudioHandle, 1,
     static_cast<int>(AudioHandleOperation::PcmClose), true},
    {"playerOpenAsset", jsAudioOpen, 2,
     static_cast<int>(AudioOpenOperation::Asset), true},
    {"sourceCreate", jsAudioOpen, 3,
     static_cast<int>(AudioOpenOperation::SourceCreate), true},
    {"sourceClose", jsAudioHandle, 1,
     static_cast<int>(AudioHandleOperation::SourceClose), true},
    {"playerOpenSource", jsAudioOpen, 2,
     static_cast<int>(AudioOpenOperation::SourcePlayer), true},
    {"playerPlay", jsAudioHandle, 1,
     static_cast<int>(AudioHandleOperation::PlayerPlay), true},
    {"playerPause", jsAudioHandle, 1,
     static_cast<int>(AudioHandleOperation::PlayerPause), true},
    {"playerSeek", jsAudioHandle, 2,
     static_cast<int>(AudioHandleOperation::PlayerSeek), true},
    {"playerSetVolume", jsAudioHandle, 2,
     static_cast<int>(AudioHandleOperation::PlayerSetVolume), true},
    {"playerSetLooping", jsAudioHandle, 2,
     static_cast<int>(AudioHandleOperation::PlayerSetLooping), true},
    {"playerStatus", jsAudioHandle, 1,
     static_cast<int>(AudioHandleOperation::PlayerStatus), true},
    {"playerClose", jsAudioHandle, 1,
     static_cast<int>(AudioHandleOperation::PlayerClose), true},
    {"playTone", jsPlayTone, 4, 0, false},
    {"recordWav", jsRecordWav, 2, 0, false},
    {"lastError", jsAudioLastError, 0, 0, false},
};

constexpr Export kCameraExports[] = {
    {"enumerate", jsCameraEnumerate, 0, 0, false},
    {"setTorch", jsCameraTorch, 2, 0, false},
    {"captureJpeg", jsCameraCapture, 6, 0, false},
    {"lastError", jsServiceLastError, 0, 0, false},
};

constexpr Export kPowerExports[] = {
    {"queryBattery", jsPowerBattery, 0, 0, true},
    {"waitForBatteryEvent", jsPowerBattery, 1, 1, true},
    {"setInteractive", jsPowerOperation, 1,
     static_cast<int>(PowerOperation::SetInteractive), true},
    {"acquireWakeLock", jsPowerOperation, 1,
     static_cast<int>(PowerOperation::AcquireWakeLock), true},
    {"releaseWakeLock", jsPowerOperation, 1,
     static_cast<int>(PowerOperation::ReleaseWakeLock), true},
    {"enableAutoSuspend", jsPowerOperation, 0,
     static_cast<int>(PowerOperation::EnableAutoSuspend), true},
    {"disableAutoSuspend", jsPowerOperation, 0,
     static_cast<int>(PowerOperation::DisableAutoSuspend), true},
    {"scheduleRtcWake", jsPowerOperation, 1,
     static_cast<int>(PowerOperation::ScheduleRtcWake), true},
    {"clearRtcWake", jsPowerOperation, 0,
     static_cast<int>(PowerOperation::ClearRtcWake), true},
    {"suspend", jsPowerOperation, 1,
     static_cast<int>(PowerOperation::Suspend), true},
    {"queryFlipState", jsPowerOperation, 0,
     static_cast<int>(PowerOperation::QueryFlipState), true},
    {"lastError", jsServiceLastError, 0, 0, false},
};

constexpr Export kVibratorExports[] = {
    {"vibrate", jsVibrator, 1,
     static_cast<int>(VibratorOperation::Vibrate), true},
    {"stop", jsVibrator, 0, static_cast<int>(VibratorOperation::Stop), true},
    {"supportsAmplitudeControl", jsVibrator, 0,
     static_cast<int>(VibratorOperation::SupportsAmplitude), true},
    {"setAmplitude", jsVibrator, 1,
     static_cast<int>(VibratorOperation::SetAmplitude), true},
    {"lastError", jsServiceLastError, 0, 0, false},
};

constexpr Export kWifiExports[] = {
    {"getStatus", jsWifiQuery, 0, static_cast<int>(WifiQuery::Status), true},
    {"scan", jsWifiQuery, 1, static_cast<int>(WifiQuery::Scan), true},
    {"listNetworks", jsWifiQuery, 0,
     static_cast<int>(WifiQuery::Networks), true},
    {"connect", jsWifiAction, 3, static_cast<int>(WifiAction::Connect), true},
    {"disconnect", jsWifiAction, 0,
     static_cast<int>(WifiAction::Disconnect), true},
    {"reconnect", jsWifiAction, 0,
     static_cast<int>(WifiAction::Reconnect), true},
    {"forget", jsWifiAction, 1, static_cast<int>(WifiAction::Forget), true},
    {"saveConfiguration", jsWifiAction, 0,
     static_cast<int>(WifiAction::Save), true},
    {"lastError", jsServiceLastError, 0, 0, false},
};

constexpr Export kIpExports[] = {
    {"getStatus", jsIpStatus, 0, 0, false},
    {"useDhcp", jsIpDhcp, 1, 0, false},
    {"useStatic", jsIpStatic, 1, 0, false},
    {"lastError", jsServiceLastError, 0, 0, false},
};

constexpr Export kBluetoothExports[] = {
    {"enable", jsBluetooth, 1,
     static_cast<int>(BluetoothOperation::Enable), true},
    {"disable", jsBluetooth, 1,
     static_cast<int>(BluetoothOperation::Disable), true},
    {"classicScan", jsBluetooth, 1,
     static_cast<int>(BluetoothOperation::ClassicScan), true},
    {"leScan", jsBluetooth, 1,
     static_cast<int>(BluetoothOperation::LeScan), true},
    {"pair", jsBluetooth, 2, static_cast<int>(BluetoothOperation::Pair), true},
    {"unpair", jsBluetooth, 1,
     static_cast<int>(BluetoothOperation::Unpair), true},
    {"cancelPairing", jsBluetooth, 1,
     static_cast<int>(BluetoothOperation::CancelPairing), true},
    {"profileConnect", jsBluetooth, 2,
     static_cast<int>(BluetoothOperation::ProfileConnect), true},
    {"profileDisconnect", jsBluetooth, 2,
     static_cast<int>(BluetoothOperation::ProfileDisconnect), true},
    {"profileConnectionCycle", jsBluetooth, 3,
     static_cast<int>(BluetoothOperation::ProfileCycle), true},
    {"leConnectionCycle", jsBluetooth, 3,
     static_cast<int>(BluetoothOperation::LeCycle), true},
    {"lastError", jsServiceLastError, 0, 0, false},
};

constexpr Export kModemExports[] = {
    {"querySnapshot", jsModemSnapshot, 1, 0, false},
    {"setRadioPower", jsModemRadioPower, 2, 0, false},
    {"lastError", jsServiceLastError, 0, 0, false},
};

constexpr Export kCodecExports[] = {
    {"testH264RoundTrip", jsCodecRoundTrip, 4, 0, false},
    {"lastError", jsServiceLastError, 0, 0, false},
};

struct Module {
  const char *name;
  const Export *exports;
  size_t export_count;
};

constexpr Module kModules[] = {
    {"oos:device", kDeviceExports, std::size(kDeviceExports)},
    {"oos:audio", kAudioExports, std::size(kAudioExports)},
    {"oos:camera", kCameraExports, std::size(kCameraExports)},
    {"oos:power", kPowerExports, std::size(kPowerExports)},
    {"oos:vibrator", kVibratorExports, std::size(kVibratorExports)},
    {"oos:wifi", kWifiExports, std::size(kWifiExports)},
    {"oos:ip", kIpExports, std::size(kIpExports)},
    {"oos:bluetooth", kBluetoothExports, std::size(kBluetoothExports)},
    {"oos:modem", kModemExports, std::size(kModemExports)},
    {"oos:codec", kCodecExports, std::size(kCodecExports)},
    {"oos:storage", kStorageExports, std::size(kStorageExports)},
    {"oos:device-storage", kDeviceStorageExports,
     std::size(kDeviceStorageExports)},
    {"oos:font-assets", kFontExports, std::size(kFontExports)},
    {"oos:assets", kAssetExports, std::size(kAssetExports)},
    {"oos:system-services", kSystemExports, std::size(kSystemExports)},
};

const Module *findModule(const char *name) {
  for (const Module &module : kModules) {
    if (std::strcmp(module.name, name) == 0)
      return &module;
  }
  return nullptr;
}

int initializeModule(JSContext *context, JSModuleDef *module) {
  JSAtom atom = JS_GetModuleName(context, module);
  const char *name = JS_AtomToCString(context, atom);
  const Module *definition = name ? findModule(name) : nullptr;
  if (name)
    JS_FreeCString(context, name);
  JS_FreeAtom(context, atom);
  if (!definition)
    return -1;
  for (size_t index = 0; index < definition->export_count; ++index) {
    const Export &entry = definition->exports[index];
    JSValue function =
        entry.uses_magic
            ? JS_NewCFunctionMagic(context, entry.magic_function, entry.name,
                                   entry.arguments, JS_CFUNC_generic_magic,
                                   entry.magic)
            : JS_NewCFunction(context, entry.function, entry.name,
                              entry.arguments);
    if (JS_SetModuleExport(context, module, entry.name, function) < 0)
      return -1;
  }
  return 0;
}

} // namespace

JSModuleDef *loadJsRuntimeModule(JSContext *context) {
  constexpr const char *exports[] = {
      "abiVersion",       "wallClockMinutes", "monotonicTimeUs",
      "wallClockTimeMs",  "wakeMainThread",   "requestExit",
      "setStatusBarStyle", "setSurfaceMode",    "log",
  };
  JSModuleDef *module =
      JS_NewCModule(context, "oos:runtime", initializeRuntimeModule);
  if (!module)
    return nullptr;
  for (const char *name : exports) {
    if (JS_AddModuleExport(context, module, name) < 0)
      return nullptr;
  }
  return module;
}

bool isJsPlatformServiceModule(const char *name) {
  return name && findModule(name);
}

JSModuleDef *loadJsPlatformServiceModule(JSContext *context, const char *name) {
  const Module *definition = findModule(name);
  if (!definition)
    return nullptr;
  JSModuleDef *module = JS_NewCModule(context, name, initializeModule);
  if (!module)
    return nullptr;
  for (size_t index = 0; index < definition->export_count; ++index) {
    if (JS_AddModuleExport(context, module,
                           definition->exports[index].name) < 0)
      return nullptr;
  }
  return module;
}

} // namespace oos::runtime
