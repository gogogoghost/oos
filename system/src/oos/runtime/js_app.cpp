#include "oos/runtime/js_app.h"

#include "oos/runtime/application_scene.h"
#include "oos/runtime/graphics_host.h"
#include "oos/runtime/js_platform_services.h"
#include "oos/runtime/module_host.h"
#include "oos/runtime/native_ui.h"
#include "oos/ui/status_bar_appearance.h"

#include <quickjs.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <limits.h>
#include <limits>
#include <new>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace oos::runtime {
namespace {

constexpr size_t kMaximumSourceBytes = 4 * 1024 * 1024;
constexpr uint32_t kMaximumPendingJobs = 4096;

uint64_t steadyMicros() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

bool regularCanonicalFile(const std::string &path, std::string &canonical) {
  char resolved[PATH_MAX];
  if (!::realpath(path.c_str(), resolved))
    return false;
  struct stat status = {};
  if (::stat(resolved, &status) != 0 || !S_ISREG(status.st_mode))
    return false;
  canonical = resolved;
  return true;
}

bool canonicalDirectory(const std::string &path, std::string &canonical) {
  char resolved[PATH_MAX];
  if (!::realpath(path.c_str(), resolved))
    return false;
  struct stat status = {};
  if (::stat(resolved, &status) != 0 || !S_ISDIR(status.st_mode))
    return false;
  canonical = resolved;
  return true;
}

bool pathUnder(const std::string &path, const std::string &root) {
  return path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
         path[root.size()] == '/';
}

std::string directoryName(const std::string &path) {
  const size_t separator = path.rfind('/');
  return separator == std::string::npos ? std::string()
                                        : path.substr(0, separator);
}

bool readSource(const char *path, std::string &source, std::string &error) {
  const int file = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (file < 0) {
    error = std::string("open JavaScript module: ") + std::strerror(errno);
    return false;
  }
  struct stat status = {};
  if (::fstat(file, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0 ||
      static_cast<uint64_t>(status.st_size) > kMaximumSourceBytes) {
    error = "JavaScript module is not a bounded regular file";
    ::close(file);
    return false;
  }
  try {
    source.resize(static_cast<size_t>(status.st_size));
  } catch (const std::bad_alloc &) {
    error = "allocate JavaScript source buffer failed";
    ::close(file);
    return false;
  }
  size_t offset = 0;
  while (offset < source.size()) {
    const ssize_t count =
        ::read(file, source.data() + offset, source.size() - offset);
    if (count > 0) {
      offset += static_cast<size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    error = count == 0 ? "JavaScript module was truncated while reading"
                       : std::string("read JavaScript module: ") +
                             std::strerror(errno);
    ::close(file);
    return false;
  }
  ::close(file);
  return true;
}

JSValue newUint8Array(JSContext *context, const uint8_t *bytes, size_t size) {
  JSValue buffer = JS_NewArrayBufferCopy(context, bytes, size);
  if (JS_IsException(buffer))
    return buffer;
  JSValue arguments[3] = {buffer, JS_UNDEFINED, JS_UNDEFINED};
  JSValue result =
      JS_NewTypedArray(context, 3, arguments, JS_TYPED_ARRAY_UINT8);
  JS_FreeValue(context, buffer);
  return result;
}

} // namespace

class JsApp::Impl final : public JsPlatformHost {
public:
  Impl(GraphicsHost &graphics, device::Device *device, JsAppOptions options)
      : graphics(graphics), device(device), options(std::move(options)) {}

  ~Impl() { shutdown(); }

  static Impl *from(JSContext *context) {
    return static_cast<Impl *>(JS_GetContextOpaque(context));
  }

  ApplicationContext *jsApplicationContext() override {
    return application.get();
  }

  bool jsRequestExit() override {
    exit_requested = true;
    return true;
  }

  static int interrupt(JSRuntime *, void *opaque) {
    auto *self = static_cast<Impl *>(opaque);
    return self && self->deadline_us != 0 &&
           steadyMicros() >= self->deadline_us;
  }

  void beginExecution() {
    deadline_us = steadyMicros() +
                  static_cast<uint64_t>(options.execution_time_limit_ms) * 1000;
  }

  void endExecution() { deadline_us = 0; }

  void captureException(std::string_view operation) {
    JSValue exception = JS_GetException(context);
    const char *message = JS_ToCString(context, exception);
    error.assign(operation);
    error += ": ";
    error += message ? message : "JavaScript exception";
    if (message)
      JS_FreeCString(context, message);
    JSValue stack = JS_GetPropertyStr(context, exception, "stack");
    if (!JS_IsException(stack) && !JS_IsUndefined(stack)) {
      const char *text = JS_ToCString(context, stack);
      if (text && error.find(text) == std::string::npos) {
        error += "\n";
        error += text;
      }
      if (text)
        JS_FreeCString(context, text);
    }
    JS_FreeValue(context, stack);
    JS_FreeValue(context, exception);
  }

  bool drainJobs(std::string_view operation) {
    uint32_t jobs = 0;
    for (;;) {
      JSContext *job_context = nullptr;
      const int result = JS_ExecutePendingJob(runtime, &job_context);
      if (result == 0)
        return true;
      if (result < 0) {
        captureException(operation);
        return false;
      }
      if (++jobs >= kMaximumPendingJobs ||
          (deadline_us != 0 && steadyMicros() >= deadline_us)) {
        error = std::string(operation) + " exceeded its execution budget";
        return false;
      }
    }
  }

  bool settle(JSValue &value, std::string_view operation) {
    if (JS_IsException(value)) {
      captureException(operation);
      return false;
    }
    if (!drainJobs(operation))
      return false;
    const JSPromiseStateEnum state = JS_PromiseState(context, value);
    if (state == JS_PROMISE_PENDING) {
      error = std::string(operation) +
              " returned a promise that cannot make progress without host I/O";
      return false;
    }
    if (state == JS_PROMISE_REJECTED) {
      JSValue reason = JS_PromiseResult(context, value);
      JS_FreeValue(context, value);
      value = JS_Throw(context, reason);
      captureException(operation);
      return false;
    }
    return true;
  }

  bool allowedModulePath(const std::string &path) const {
    return pathUnder(path, application_root) || pathUnder(path, module_root);
  }

  bool resolveModule(const char *base_name, const char *name,
                     std::string &resolved) {
    if (!name || !name[0]) {
      error = "empty JavaScript module specifier";
      return false;
    }
    const std::string_view specifier(name);
    if (specifier.rfind("oos:", 0) == 0) {
      resolved.assign(specifier);
      return true;
    }

    std::string candidate;
    if (specifier[0] == '.') {
      if (!base_name || base_name[0] == '\0' ||
          std::string_view(base_name).rfind("oos:", 0) == 0) {
        error = "relative import has no package module base";
        return false;
      }
      candidate = directoryName(base_name) + "/" + std::string(specifier);
    } else if (specifier[0] == '/') {
      error = "absolute JavaScript imports are not allowed";
      return false;
    } else {
      const apps::AppModule *declared = nullptr;
      for (const apps::AppModule &module : options.modules) {
        if (module.name == specifier) {
          declared = &module;
          break;
        }
      }
      if (!declared || declared->runtime != apps::AppRuntimeKind::JavaScript) {
        error = "JavaScript import is not a declared JS module: " +
                std::string(specifier);
        return false;
      }
      constexpr std::string_view prefix = apps::kModulePrefix;
      if (declared->path.rfind(prefix, 0) != 0) {
        error = "declared JavaScript module path is invalid";
        return false;
      }
      candidate = module_root + "/" + declared->path.substr(prefix.size());
    }
    if (!regularCanonicalFile(candidate, resolved) ||
        !allowedModulePath(resolved) ||
        !(resolved.size() >= 3 &&
          (resolved.compare(resolved.size() - 3, 3, ".js") == 0 ||
           (resolved.size() >= 4 &&
            resolved.compare(resolved.size() - 4, 4, ".mjs") == 0)))) {
      error = "JavaScript import escapes the package or is not a JS module";
      return false;
    }
    return true;
  }

  static char *normalizeModule(JSContext *context, const char *base_name,
                               const char *name, void *opaque) {
    auto *self = static_cast<Impl *>(opaque);
    std::string resolved;
    if (!self || !self->resolveModule(base_name, name, resolved)) {
      JS_ThrowReferenceError(context, "%s",
                             self ? self->error.c_str()
                                  : "JavaScript module loader unavailable");
      return nullptr;
    }
    return js_strdup(context, resolved.c_str());
  }

  bool setImportMeta(JSModuleDef *module, bool main) {
    JSValue meta = JS_GetImportMeta(context, module);
    if (JS_IsException(meta))
      return false;
    JSAtom atom = JS_GetModuleName(context, module);
    const char *name = JS_AtomToCString(context, atom);
    std::string url = "oos-app:";
    if (name) {
      if (std::string_view(name).rfind(application_root, 0) == 0)
        url += std::string_view(name).substr(application_root.size());
      else if (std::string_view(name).rfind(module_root, 0) == 0)
        url += "/modules" +
               std::string(std::string_view(name).substr(module_root.size()));
      else
        url += name;
    }
    if (name)
      JS_FreeCString(context, name);
    JS_FreeAtom(context, atom);
    const bool ok =
        JS_SetPropertyStr(context, meta, "url",
                          JS_NewString(context, url.c_str())) >= 0 &&
        JS_SetPropertyStr(context, meta, "main", JS_NewBool(context, main)) >=
            0;
    JS_FreeValue(context, meta);
    return ok;
  }

  static JSModuleDef *loadModule(JSContext *context, const char *name,
                                 void *opaque) {
    auto *self = static_cast<Impl *>(opaque);
    if (!self)
      return nullptr;
    if (std::string_view(name).rfind("oos:", 0) == 0)
      return self->loadBuiltinModule(name);
    std::string source;
    if (!readSource(name, source, self->error)) {
      JS_ThrowReferenceError(context, "%s", self->error.c_str());
      return nullptr;
    }
    JSValue compiled = JS_Eval(context, source.data(), source.size(), name,
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled))
      return nullptr;
    auto *module = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(compiled));
    if (!self->setImportMeta(module, false)) {
      JS_FreeValue(context, compiled);
      return nullptr;
    }
    JS_FreeValue(context, compiled);
    return module;
  }

  static JSValue jsLog(JSContext *context, JSValueConst, int argc,
                       JSValueConst *argv, int magic) {
    const char *level = magic == 2 ? "error" : magic == 1 ? "warn" : "info";
    std::fprintf(stderr, "[js:%s]", level);
    for (int index = 0; index < argc; ++index) {
      const char *value = JS_ToCString(context, argv[index]);
      std::fprintf(stderr, "%s%s", index == 0 ? " " : " ",
                   value ? value : "<value>");
      if (value)
        JS_FreeCString(context, value);
    }
    std::fputc('\n', stderr);
    return JS_UNDEFINED;
  }

  static bool numberProperty(JSContext *context, JSValueConst object,
                             const char *name, float &value,
                             float default_value = 0) {
    JSValue property = JS_GetPropertyStr(context, object, name);
    if (JS_IsException(property))
      return false;
    if (JS_IsUndefined(property)) {
      value = default_value;
      JS_FreeValue(context, property);
      return true;
    }
    double number = 0;
    const bool success = JS_ToFloat64(context, &number, property) == 0 &&
                         std::isfinite(number) &&
                         number >= -std::numeric_limits<float>::max() &&
                         number <= std::numeric_limits<float>::max();
    JS_FreeValue(context, property);
    if (success)
      value = static_cast<float>(number);
    return success;
  }

  static bool uintProperty(JSContext *context, JSValueConst object,
                           const char *name, uint32_t &value,
                           uint32_t default_value = 0) {
    JSValue property = JS_GetPropertyStr(context, object, name);
    if (JS_IsException(property))
      return false;
    if (JS_IsUndefined(property)) {
      value = default_value;
      JS_FreeValue(context, property);
      return true;
    }
    const bool success = JS_ToUint32(context, &value, property) == 0;
    JS_FreeValue(context, property);
    return success;
  }

  static bool stringProperty(JSContext *context, JSValueConst object,
                             const char *name, std::string &value,
                             bool required = false) {
    JSValue property = JS_GetPropertyStr(context, object, name);
    if (JS_IsException(property))
      return false;
    if (JS_IsUndefined(property) || JS_IsNull(property)) {
      JS_FreeValue(context, property);
      value.clear();
      return !required;
    }
    size_t size = 0;
    const char *text = JS_ToCStringLen(context, &size, property);
    if (!text) {
      JS_FreeValue(context, property);
      return false;
    }
    value.assign(text, size);
    JS_FreeCString(context, text);
    JS_FreeValue(context, property);
    return true;
  }

  static bool arrayLength(JSContext *context, JSValueConst array,
                          uint32_t &length) {
    if (JS_IsArray(context, array) != 1)
      return false;
    JSValue property = JS_GetPropertyStr(context, array, "length");
    if (JS_IsException(property))
      return false;
    const bool success = JS_ToUint32(context, &length, property) == 0;
    JS_FreeValue(context, property);
    return success;
  }

  static JSValue jsCanvasCreate(JSContext *context, JSValueConst, int argc,
                                JSValueConst *argv) {
    Impl *self = from(context);
    if (!self || !self->scene || argc < 1 || !JS_IsObject(argv[0]))
      return JS_ThrowTypeError(context,
                               "createCanvas expects an options object");
    std::string context_name;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z_bits = 0;
    int visible = 1;
    if (!stringProperty(context, argv[0], "context", context_name, true) ||
        !uintProperty(context, argv[0], "width", width) ||
        !uintProperty(context, argv[0], "height", height) ||
        !uintProperty(context, argv[0], "x", x) ||
        !uintProperty(context, argv[0], "y", y) ||
        !uintProperty(context, argv[0], "z", z_bits))
      return JS_ThrowTypeError(context, "invalid canvas options");
    JSValue visible_value = JS_GetPropertyStr(context, argv[0], "visible");
    if (!JS_IsUndefined(visible_value))
      visible = JS_ToBool(context, visible_value);
    JS_FreeValue(context, visible_value);
    if (visible < 0)
      return JS_EXCEPTION;
    CanvasContextKind kind = CanvasContextKind::None;
    if (context_name == "2d")
      kind = CanvasContextKind::Canvas2d;
    else if (context_name == "mesh2d")
      kind = CanvasContextKind::Mesh2d;
    else if (context_name == "webgl")
      kind = CanvasContextKind::Gles2;
    if (kind == CanvasContextKind::None)
      return JS_ThrowRangeError(
          context, "canvas context must be '2d', 'mesh2d', or 'webgl'");
    const uint32_t handle = self->scene->createCanvas(
        {static_cast<int32_t>(x), static_cast<int32_t>(y), width, height,
         static_cast<int32_t>(z_bits), visible != 0},
        kind);
    if (handle == 0)
      return JS_ThrowInternalError(context, "%s",
                                   self->scene->lastError().c_str());
    return JS_NewUint32(context, handle);
  }

  static JSValue jsCanvasConfigure(JSContext *context, JSValueConst, int argc,
                                   JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t handle = 0;
    if (!self || !self->scene || argc < 2 ||
        JS_ToUint32(context, &handle, argv[0]) < 0 || !JS_IsObject(argv[1]))
      return JS_ThrowTypeError(context,
                               "configureCanvas expects a handle and options");
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
    if (!uintProperty(context, argv[1], "width", width) ||
        !uintProperty(context, argv[1], "height", height) ||
        !uintProperty(context, argv[1], "x", x) ||
        !uintProperty(context, argv[1], "y", y) ||
        !uintProperty(context, argv[1], "z", z))
      return JS_ThrowTypeError(context, "invalid canvas geometry");
    JSValue visible_value = JS_GetPropertyStr(context, argv[1], "visible");
    const int visible =
        JS_IsUndefined(visible_value) ? 1 : JS_ToBool(context, visible_value);
    JS_FreeValue(context, visible_value);
    if (visible < 0)
      return JS_EXCEPTION;
    if (!self->scene->configureCanvas(
            handle, {static_cast<int32_t>(x), static_cast<int32_t>(y), width,
                     height, static_cast<int32_t>(z), visible != 0}))
      return JS_ThrowRangeError(context, "%s",
                                self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsCanvasDestroy(JSContext *context, JSValueConst, int argc,
                                 JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t handle = 0;
    if (!self || !self->scene || argc < 1 ||
        JS_ToUint32(context, &handle, argv[0]) < 0)
      return JS_ThrowTypeError(context, "destroyCanvas expects a handle");
    if (!self->scene->destroyCanvas(handle))
      return JS_ThrowRangeError(context, "%s",
                                self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsCanvasSubmit2d(JSContext *context, JSValueConst, int argc,
                                  JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t handle = 0;
    uint32_t count = 0;
    if (!self || !self->scene || argc < 2 ||
        JS_ToUint32(context, &handle, argv[0]) < 0 ||
        !arrayLength(context, argv[1], count) || count > 8192)
      return JS_ThrowTypeError(
          context, "submitCanvas2d expects a handle and command array");
    std::vector<Canvas2dCommand> commands;
    std::vector<uint8_t> text;
    try {
      commands.reserve(count);
    } catch (const std::bad_alloc &) {
      return JS_ThrowOutOfMemory(context);
    }
    for (uint32_t index = 0; index < count; ++index) {
      JSValue object = JS_GetPropertyUint32(context, argv[1], index);
      if (JS_IsException(object) || !JS_IsObject(object)) {
        JS_FreeValue(context, object);
        return JS_ThrowTypeError(context, "Canvas2D command must be an object");
      }
      std::string operation;
      Canvas2dCommand command;
      uint32_t color = 0;
      const bool valid =
          stringProperty(context, object, "op", operation, true) &&
          numberProperty(context, object, "x", command.x) &&
          numberProperty(context, object, "y", command.y) &&
          numberProperty(context, object, "width", command.width) &&
          numberProperty(context, object, "height", command.height) &&
          numberProperty(context, object, "radius", command.radius) &&
          numberProperty(context, object, "lineWidth", command.line_width, 1) &&
          numberProperty(context, object, "fontSize", command.font_size, 14) &&
          uintProperty(context, object, "rgba", color);
      command.rgba = color;
      if (operation == "clear")
        command.opcode = static_cast<uint8_t>(Canvas2dOpcode::Clear);
      else if (operation == "fillRect")
        command.opcode = static_cast<uint8_t>(Canvas2dOpcode::FillRect);
      else if (operation == "strokeRect")
        command.opcode = static_cast<uint8_t>(Canvas2dOpcode::StrokeRect);
      else if (operation == "pushClip")
        command.opcode = static_cast<uint8_t>(Canvas2dOpcode::PushClip);
      else if (operation == "popClip")
        command.opcode = static_cast<uint8_t>(Canvas2dOpcode::PopClip);
      else if (operation == "fillText") {
        command.opcode = static_cast<uint8_t>(Canvas2dOpcode::FillText);
        std::string value;
        if (!stringProperty(context, object, "text", value, true) ||
            value.size() > UINT32_MAX - text.size()) {
          JS_FreeValue(context, object);
          return JS_ThrowTypeError(context, "invalid Canvas2D text command");
        }
        command.text_offset = static_cast<uint32_t>(text.size());
        command.text_length = static_cast<uint32_t>(value.size());
        text.insert(text.end(), value.begin(), value.end());
      } else {
        JS_FreeValue(context, object);
        return JS_ThrowRangeError(context, "unknown Canvas2D operation");
      }
      JS_FreeValue(context, object);
      if (!valid)
        return JS_ThrowTypeError(context, "invalid Canvas2D command fields");
      commands.push_back(command);
    }
    if (!self->scene->submitCanvas2d(
            handle, commands.empty() ? nullptr : commands.data(),
            commands.size(), text.empty() ? nullptr : text.data(), text.size()))
      return JS_ThrowInternalError(context, "%s",
                                   self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static bool textureDescriptor(JSContext *context, JSValueConst descriptor,
                                uint32_t &format, uint32_t &x, uint32_t &y,
                                uint32_t &width, uint32_t &height,
                                uint32_t &row_stride, uint32_t &flags) {
    return JS_IsObject(descriptor) &&
           uintProperty(context, descriptor, "format", format) &&
           uintProperty(context, descriptor, "x", x) &&
           uintProperty(context, descriptor, "y", y) &&
           uintProperty(context, descriptor, "width", width) &&
           uintProperty(context, descriptor, "height", height) &&
           uintProperty(context, descriptor, "rowStride", row_stride) &&
           uintProperty(context, descriptor, "flags", flags);
  }

  static JSValue jsCanvasTextureSet(JSContext *context, JSValueConst, int argc,
                                    JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t canvas = 0;
    uint32_t texture = 0;
    uint32_t format = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t row_stride = 0;
    uint32_t flags = 0;
    const uint8_t *pixels = nullptr;
    size_t pixel_size = 0;
    JSValue backing = JS_UNDEFINED;
    const bool valid = self && self->scene && argc >= 4 &&
                       JS_ToUint32(context, &canvas, argv[0]) == 0 &&
                       JS_ToUint32(context, &texture, argv[1]) == 0 &&
                       textureDescriptor(context, argv[2], format, x, y, width,
                                         height, row_stride, flags) &&
                       byteView(context, argv[3], pixels, pixel_size, backing);
    const bool success =
        valid && self->scene->setCanvasTexture(canvas, texture, format, x, y,
                                               width, height, row_stride, flags,
                                               pixels, pixel_size);
    JS_FreeValue(context, backing);
    if (!valid)
      return JS_ThrowTypeError(context, "invalid mesh texture upload");
    if (!success)
      return JS_ThrowRangeError(context, "%s",
                                self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsCanvasTextureFree(JSContext *context, JSValueConst, int argc,
                                     JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t canvas = 0;
    uint32_t texture = 0;
    if (!self || !self->scene || argc < 2 ||
        JS_ToUint32(context, &canvas, argv[0]) < 0 ||
        JS_ToUint32(context, &texture, argv[1]) < 0)
      return JS_ThrowTypeError(
          context, "textureFree expects canvas and texture handles");
    if (!self->scene->freeCanvasTexture(canvas, texture))
      return JS_ThrowRangeError(context, "%s",
                                self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsCanvasSubmitMesh(JSContext *context, JSValueConst, int argc,
                                    JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t canvas = 0;
    uint32_t vertex_count = 0;
    uint32_t command_count = 0;
    uint32_t clear = 0;
    if (!self || !self->scene || argc < 5 ||
        JS_ToUint32(context, &canvas, argv[0]) < 0 ||
        !arrayLength(context, argv[1], vertex_count) ||
        !arrayLength(context, argv[3], command_count) ||
        JS_ToUint32(context, &clear, argv[4]) < 0 ||
        vertex_count > OOS_GFX_MAX_VERTICES ||
        command_count > OOS_GFX_MAX_DRAW_COMMANDS)
      return JS_ThrowTypeError(context, "invalid mesh batch");

    std::vector<OosGfxVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<OosGfxDrawCommand> commands;
    try {
      vertices.reserve(vertex_count);
      commands.reserve(command_count);
    } catch (const std::bad_alloc &) {
      return JS_ThrowOutOfMemory(context);
    }
    for (uint32_t index = 0; index < vertex_count; ++index) {
      JSValue object = JS_GetPropertyUint32(context, argv[1], index);
      OosGfxVertex vertex = {};
      float red = 255;
      float green = 255;
      float blue = 255;
      float alpha = 255;
      const bool valid =
          JS_IsObject(object) &&
          numberProperty(context, object, "x", vertex.position[0]) &&
          numberProperty(context, object, "y", vertex.position[1]) &&
          numberProperty(context, object, "u", vertex.uv[0]) &&
          numberProperty(context, object, "v", vertex.uv[1]) &&
          numberProperty(context, object, "r", red, 255) &&
          numberProperty(context, object, "g", green, 255) &&
          numberProperty(context, object, "b", blue, 255) &&
          numberProperty(context, object, "a", alpha, 255) && red >= 0 &&
          red <= 255 && green >= 0 && green <= 255 && blue >= 0 &&
          blue <= 255 && alpha >= 0 && alpha <= 255;
      JS_FreeValue(context, object);
      if (!valid)
        return JS_ThrowTypeError(context, "invalid mesh vertex");
      vertex.color[0] = static_cast<uint8_t>(red);
      vertex.color[1] = static_cast<uint8_t>(green);
      vertex.color[2] = static_cast<uint8_t>(blue);
      vertex.color[3] = static_cast<uint8_t>(alpha);
      vertices.push_back(vertex);
    }
    const uint8_t *index_bytes = nullptr;
    size_t index_size = 0;
    JSValue index_backing = JS_UNDEFINED;
    if (!byteView(context, argv[2], index_bytes, index_size, index_backing) ||
        index_size % sizeof(uint16_t) != 0 ||
        index_size / sizeof(uint16_t) > OOS_GFX_MAX_INDICES) {
      JS_FreeValue(context, index_backing);
      return JS_ThrowTypeError(context,
                               "mesh indices must be a bounded Uint16Array");
    }
    try {
      indices.resize(index_size / sizeof(uint16_t));
    } catch (const std::bad_alloc &) {
      JS_FreeValue(context, index_backing);
      return JS_ThrowOutOfMemory(context);
    }
    if (index_size)
      std::memcpy(indices.data(), index_bytes, index_size);
    JS_FreeValue(context, index_backing);

    for (uint32_t index = 0; index < command_count; ++index) {
      JSValue object = JS_GetPropertyUint32(context, argv[3], index);
      OosGfxDrawCommand command = {};
      const bool valid =
          JS_IsObject(object) &&
          uintProperty(context, object, "firstIndex", command.first_index) &&
          uintProperty(context, object, "indexCount", command.index_count) &&
          uintProperty(context, object, "texture", command.texture) &&
          numberProperty(context, object, "clipMinX", command.clip_min[0]) &&
          numberProperty(context, object, "clipMinY", command.clip_min[1]) &&
          numberProperty(context, object, "clipMaxX", command.clip_max[0]) &&
          numberProperty(context, object, "clipMaxY", command.clip_max[1]);
      JS_FreeValue(context, object);
      if (!valid)
        return JS_ThrowTypeError(context, "invalid mesh draw command");
      commands.push_back(command);
    }
    if (!self->scene->submitCanvasMesh(
            canvas, vertices.empty() ? nullptr : vertices.data(),
            vertices.size(), indices.empty() ? nullptr : indices.data(),
            indices.size(), commands.empty() ? nullptr : commands.data(),
            commands.size(), clear))
      return JS_ThrowRangeError(context, "%s",
                                self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsGraphicsSurfaceSize(JSContext *context, JSValueConst, int,
                                       JSValueConst *) {
    Impl *self = from(context);
    if (!self || !self->scene)
      return JS_ThrowInternalError(context,
                                   "application surface is unavailable");
    JSValue result = JS_NewObject(context);
    if (JS_IsException(result))
      return result;
    if (JS_SetPropertyStr(context, result, "width",
                          JS_NewUint32(context, self->scene->width())) < 0 ||
        JS_SetPropertyStr(context, result, "height",
                          JS_NewUint32(context, self->scene->height())) < 0) {
      JS_FreeValue(context, result);
      return JS_EXCEPTION;
    }
    return result;
  }

  static JSValue jsGraphicsPixelsPerPoint(JSContext *context, JSValueConst, int,
                                          JSValueConst *) {
    Impl *self = from(context);
    return self && self->scene
               ? JS_NewFloat64(context, self->scene->pixelsPerPoint())
               : JS_ThrowInternalError(context,
                                       "application surface is unavailable");
  }

  static JSValue jsGraphicsSurfaceFormat(JSContext *context, JSValueConst, int,
                                         JSValueConst *) {
    Impl *self = from(context);
    return self && self->scene
               ? JS_NewUint32(context, self->scene->surfaceFormat())
               : JS_ThrowInternalError(context,
                                       "application surface is unavailable");
  }

  static JSValue jsGraphicsSupportedTextureFormats(JSContext *context,
                                                   JSValueConst, int,
                                                   JSValueConst *) {
    Impl *self = from(context);
    return self && self->scene
               ? JS_NewUint32(context, self->scene->supportedTextureFormats())
               : JS_ThrowInternalError(context,
                                       "application surface is unavailable");
  }

  static JSValue jsGraphicsLimits(JSContext *context, JSValueConst, int,
                                  JSValueConst *) {
    JSValue result = JS_NewObject(context);
    if (JS_IsException(result))
      return result;
    const struct Field {
      const char *name;
      uint32_t value;
    } fields[] = {
        {"maxTextureSize", OOS_GFX_MAX_TEXTURE_SIZE},
        {"maxTextureBytes", OOS_GFX_MAX_TEXTURE_BYTES},
        {"maxVertices", OOS_GFX_MAX_VERTICES},
        {"maxIndices", OOS_GFX_MAX_INDICES},
        {"maxDrawCommands", OOS_GFX_MAX_DRAW_COMMANDS},
    };
    for (const Field &field : fields) {
      if (JS_SetPropertyStr(context, result, field.name,
                            JS_NewUint32(context, field.value)) < 0) {
        JS_FreeValue(context, result);
        return JS_EXCEPTION;
      }
    }
    return result;
  }

  static JSValue jsGraphicsTextureSet(JSContext *context, JSValueConst,
                                      int argc, JSValueConst *argv) {
    if (argc < 3)
      return JS_ThrowTypeError(
          context, "textureSet expects a texture, descriptor, and pixels");
    JSValue root = JS_NewUint32(context, 0);
    JSValueConst forwarded[] = {root, argv[0], argv[1], argv[2]};
    JSValue result =
        jsCanvasTextureSet(context, JS_UNDEFINED,
                           static_cast<int>(std::size(forwarded)), forwarded);
    JS_FreeValue(context, root);
    return result;
  }

  static JSValue jsGraphicsTextureFree(JSContext *context, JSValueConst,
                                       int argc, JSValueConst *argv) {
    if (argc < 1)
      return JS_ThrowTypeError(context, "textureFree expects a texture handle");
    JSValue root = JS_NewUint32(context, 0);
    JSValueConst forwarded[] = {root, argv[0]};
    JSValue result =
        jsCanvasTextureFree(context, JS_UNDEFINED,
                            static_cast<int>(std::size(forwarded)), forwarded);
    JS_FreeValue(context, root);
    return result;
  }

  static JSValue jsGraphicsSubmit(JSContext *context, JSValueConst, int argc,
                                  JSValueConst *argv) {
    if (argc < 4)
      return JS_ThrowTypeError(
          context,
          "submit expects vertices, indices, commands, and clear color");
    JSValue root = JS_NewUint32(context, 0);
    JSValueConst forwarded[] = {root, argv[0], argv[1], argv[2], argv[3]};
    JSValue result =
        jsCanvasSubmitMesh(context, JS_UNDEFINED,
                           static_cast<int>(std::size(forwarded)), forwarded);
    JS_FreeValue(context, root);
    return result;
  }

  static JSValue jsGlesCapabilities(JSContext *context, JSValueConst, int argc,
                                    JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t canvas = 0;
    OosGlesCapabilities capabilities = {};
    if (!self || !self->scene || argc < 1 ||
        JS_ToUint32(context, &canvas, argv[0]) < 0 ||
        !self->scene->canvasGlesCapabilities(canvas, capabilities))
      return JS_ThrowRangeError(context, "GLES canvas is unavailable");
    JSValue result = JS_NewObject(context);
    const struct Field {
      const char *name;
      uint32_t value;
    } fields[] = {
        {"majorVersion", capabilities.major_version},
        {"minorVersion", capabilities.minor_version},
        {"maxTextureSize", capabilities.max_texture_size},
        {"maxTextureUnits", capabilities.max_texture_units},
        {"maxVertexAttributes", capabilities.max_vertex_attributes},
        {"maxVaryingVectors", capabilities.max_varying_vectors},
        {"maxVertexUniformVectors", capabilities.max_vertex_uniform_vectors},
        {"maxFragmentUniformVectors",
         capabilities.max_fragment_uniform_vectors},
        {"depthBits", capabilities.depth_bits},
        {"stencilBits", capabilities.stencil_bits},
        {"maxBufferBytes", capabilities.max_buffer_bytes},
        {"maxCommands", capabilities.max_commands},
        {"maxCommandDataWords", capabilities.max_command_data_words},
    };
    for (const Field &field : fields)
      JS_SetPropertyStr(context, result, field.name,
                        JS_NewUint32(context, field.value));
    return result;
  }

  static JSValue jsGlesTextureSet(JSContext *context, JSValueConst, int argc,
                                  JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t canvas = 0;
    uint32_t texture = 0;
    uint32_t format = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t row_stride = 0;
    uint32_t flags = 0;
    const uint8_t *pixels = nullptr;
    size_t pixel_size = 0;
    JSValue backing = JS_UNDEFINED;
    const bool valid = self && self->scene && argc >= 4 &&
                       JS_ToUint32(context, &canvas, argv[0]) == 0 &&
                       JS_ToUint32(context, &texture, argv[1]) == 0 &&
                       textureDescriptor(context, argv[2], format, x, y, width,
                                         height, row_stride, flags) &&
                       byteView(context, argv[3], pixels, pixel_size, backing);
    const bool success =
        valid && self->scene->setCanvasGlesTexture(canvas, texture, format, x,
                                                   y, width, height, row_stride,
                                                   flags, pixels, pixel_size);
    JS_FreeValue(context, backing);
    if (!valid)
      return JS_ThrowTypeError(context, "invalid GLES texture upload");
    if (!success)
      return JS_ThrowRangeError(context, "%s",
                                self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsGlesTextureFree(JSContext *context, JSValueConst, int argc,
                                   JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t canvas = 0;
    uint32_t texture = 0;
    if (!self || !self->scene || argc < 2 ||
        JS_ToUint32(context, &canvas, argv[0]) < 0 ||
        JS_ToUint32(context, &texture, argv[1]) < 0)
      return JS_ThrowTypeError(
          context, "textureFree expects canvas and texture handles");
    if (!self->scene->freeCanvasGlesTexture(canvas, texture))
      return JS_ThrowRangeError(context, "%s",
                                self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsGlesBufferSet(JSContext *context, JSValueConst, int argc,
                                 JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t canvas = 0;
    uint32_t buffer = 0;
    uint32_t size = 0;
    uint32_t usage = 0;
    const uint8_t *data = nullptr;
    size_t data_size = 0;
    JSValue backing = JS_UNDEFINED;
    const bool valid = self && self->scene && argc >= 5 &&
                       JS_ToUint32(context, &canvas, argv[0]) == 0 &&
                       JS_ToUint32(context, &buffer, argv[1]) == 0 &&
                       JS_ToUint32(context, &size, argv[2]) == 0 &&
                       JS_ToUint32(context, &usage, argv[3]) == 0 &&
                       byteView(context, argv[4], data, data_size, backing);
    const bool success = valid && self->scene->setCanvasGlesBuffer(
                                      canvas, buffer, size, usage,
                                      data_size ? data : nullptr, data_size);
    JS_FreeValue(context, backing);
    if (!valid)
      return JS_ThrowTypeError(context, "invalid GLES buffer upload");
    if (!success)
      return JS_ThrowRangeError(context, "%s",
                                self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsGlesBufferWrite(JSContext *context, JSValueConst, int argc,
                                   JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t canvas = 0;
    uint32_t buffer = 0;
    uint32_t offset = 0;
    const uint8_t *data = nullptr;
    size_t data_size = 0;
    JSValue backing = JS_UNDEFINED;
    const bool valid = self && self->scene && argc >= 4 &&
                       JS_ToUint32(context, &canvas, argv[0]) == 0 &&
                       JS_ToUint32(context, &buffer, argv[1]) == 0 &&
                       JS_ToUint32(context, &offset, argv[2]) == 0 &&
                       byteView(context, argv[3], data, data_size, backing);
    const bool success = valid && data_size != 0 &&
                         self->scene->writeCanvasGlesBuffer(
                             canvas, buffer, offset, data, data_size);
    JS_FreeValue(context, backing);
    if (!valid || data_size == 0)
      return JS_ThrowTypeError(context, "invalid GLES buffer write");
    if (!success)
      return JS_ThrowRangeError(context, "%s",
                                self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsGlesBufferFree(JSContext *context, JSValueConst, int argc,
                                  JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t canvas = 0;
    uint32_t buffer = 0;
    if (!self || !self->scene || argc < 2 ||
        JS_ToUint32(context, &canvas, argv[0]) < 0 ||
        JS_ToUint32(context, &buffer, argv[1]) < 0)
      return JS_ThrowTypeError(context,
                               "bufferFree expects canvas and buffer handles");
    if (!self->scene->freeCanvasGlesBuffer(canvas, buffer))
      return JS_ThrowRangeError(context, "%s",
                                self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsGlesShaderSet(JSContext *context, JSValueConst, int argc,
                                 JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t canvas = 0;
    uint32_t shader = 0;
    uint32_t stage = 0;
    size_t source_size = 0;
    const char *source =
        argc >= 4 ? JS_ToCStringLen(context, &source_size, argv[3]) : nullptr;
    const bool valid = self && self->scene && source &&
                       JS_ToUint32(context, &canvas, argv[0]) == 0 &&
                       JS_ToUint32(context, &shader, argv[1]) == 0 &&
                       JS_ToUint32(context, &stage, argv[2]) == 0;
    const bool success =
        valid && self->scene->setCanvasGlesShader(canvas, shader, stage, source,
                                                  source_size);
    if (source)
      JS_FreeCString(context, source);
    if (!valid)
      return JS_ThrowTypeError(context, "invalid GLES shader");
    if (!success)
      return JS_ThrowRangeError(context, "%s",
                                self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsGlesShaderFree(JSContext *context, JSValueConst, int argc,
                                  JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t canvas = 0;
    uint32_t shader = 0;
    if (!self || !self->scene || argc < 2 ||
        JS_ToUint32(context, &canvas, argv[0]) < 0 ||
        JS_ToUint32(context, &shader, argv[1]) < 0)
      return JS_ThrowTypeError(context,
                               "shaderFree expects canvas and shader handles");
    if (!self->scene->freeCanvasGlesShader(canvas, shader))
      return JS_ThrowRangeError(context, "%s",
                                self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsGlesProgramSet(JSContext *context, JSValueConst, int argc,
                                  JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t canvas = 0;
    uint32_t program = 0;
    uint32_t vertex = 0;
    uint32_t fragment = 0;
    if (!self || !self->scene || argc < 4 ||
        JS_ToUint32(context, &canvas, argv[0]) < 0 ||
        JS_ToUint32(context, &program, argv[1]) < 0 ||
        JS_ToUint32(context, &vertex, argv[2]) < 0 ||
        JS_ToUint32(context, &fragment, argv[3]) < 0)
      return JS_ThrowTypeError(context, "invalid GLES program");
    if (!self->scene->setCanvasGlesProgram(canvas, program, vertex, fragment))
      return JS_ThrowRangeError(context, "%s",
                                self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsGlesProgramFree(JSContext *context, JSValueConst, int argc,
                                   JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t canvas = 0;
    uint32_t program = 0;
    if (!self || !self->scene || argc < 2 ||
        JS_ToUint32(context, &canvas, argv[0]) < 0 ||
        JS_ToUint32(context, &program, argv[1]) < 0)
      return JS_ThrowTypeError(
          context, "programFree expects canvas and program handles");
    if (!self->scene->freeCanvasGlesProgram(canvas, program))
      return JS_ThrowRangeError(context, "%s",
                                self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsGlesLocation(JSContext *context, JSValueConst, int argc,
                                JSValueConst *argv, int magic) {
    Impl *self = from(context);
    uint32_t canvas = 0;
    uint32_t program = 0;
    size_t name_size = 0;
    const char *name =
        argc >= 3 ? JS_ToCStringLen(context, &name_size, argv[2]) : nullptr;
    if (!self || !self->scene || !name ||
        JS_ToUint32(context, &canvas, argv[0]) < 0 ||
        JS_ToUint32(context, &program, argv[1]) < 0) {
      if (name)
        JS_FreeCString(context, name);
      return JS_ThrowTypeError(context, "invalid GLES location query");
    }
    const int32_t location = magic ? self->scene->canvasGlesUniformLocation(
                                         canvas, program, name, name_size)
                                   : self->scene->canvasGlesAttributeLocation(
                                         canvas, program, name, name_size);
    JS_FreeCString(context, name);
    return JS_NewInt32(context, location);
  }

  static JSValue jsGlesSubmit(JSContext *context, JSValueConst, int argc,
                              JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t canvas = 0;
    uint32_t command_count = 0;
    if (!self || !self->scene || argc < 3 ||
        JS_ToUint32(context, &canvas, argv[0]) < 0 ||
        !arrayLength(context, argv[1], command_count) ||
        command_count > OOS_GLES_MAX_COMMANDS)
      return JS_ThrowTypeError(context, "invalid GLES command batch");
    std::vector<OosGlesCommand> commands;
    try {
      commands.reserve(command_count);
    } catch (const std::bad_alloc &) {
      return JS_ThrowOutOfMemory(context);
    }
    for (uint32_t index = 0; index < command_count; ++index) {
      JSValue object = JS_GetPropertyUint32(context, argv[1], index);
      OosGlesCommand command = {};
      uint32_t opcode = 0;
      bool valid =
          JS_IsObject(object) && uintProperty(context, object, "op", opcode);
      for (uint32_t argument = 0; valid && argument < 8; ++argument) {
        const std::string name = "a" + std::to_string(argument);
        valid =
            uintProperty(context, object, name.c_str(), command.args[argument]);
      }
      JS_FreeValue(context, object);
      if (!valid || opcode > OOS_GLES_END_FRAME)
        return JS_ThrowTypeError(context, "invalid GLES command");
      command.opcode = static_cast<uint8_t>(opcode);
      commands.push_back(command);
    }
    const uint8_t *word_bytes = nullptr;
    size_t word_byte_size = 0;
    JSValue backing = JS_UNDEFINED;
    if (!byteView(context, argv[2], word_bytes, word_byte_size, backing) ||
        word_byte_size % sizeof(uint32_t) != 0 ||
        word_byte_size / sizeof(uint32_t) > OOS_GLES_MAX_COMMAND_DATA_WORDS) {
      JS_FreeValue(context, backing);
      return JS_ThrowTypeError(context,
                               "GLES data must be a bounded Uint32Array");
    }
    std::vector<uint32_t> words;
    try {
      words.resize(word_byte_size / sizeof(uint32_t));
    } catch (const std::bad_alloc &) {
      JS_FreeValue(context, backing);
      return JS_ThrowOutOfMemory(context);
    }
    if (word_byte_size)
      std::memcpy(words.data(), word_bytes, word_byte_size);
    JS_FreeValue(context, backing);
    if (!self->scene->submitCanvasGles(
            canvas, commands.empty() ? nullptr : commands.data(),
            commands.size(), words.empty() ? nullptr : words.data(),
            words.size()))
      return JS_ThrowRangeError(context, "%s",
                                self->scene->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsUiSubmit(JSContext *context, JSValueConst, int argc,
                            JSValueConst *argv) {
    Impl *self = from(context);
    uint32_t count = 0;
    if (!self || !self->ui || argc < 1 ||
        !arrayLength(context, argv[0], count) || count == 0 || count > 4096)
      return JS_ThrowTypeError(context,
                               "ui.submit expects a non-empty node array");
    std::vector<UiNodeRecord> records;
    std::vector<uint8_t> strings;
    try {
      records.reserve(count);
    } catch (const std::bad_alloc &) {
      return JS_ThrowOutOfMemory(context);
    }
    for (uint32_t index = 0; index < count; ++index) {
      JSValue object = JS_GetPropertyUint32(context, argv[0], index);
      if (JS_IsException(object) || !JS_IsObject(object)) {
        JS_FreeValue(context, object);
        return JS_ThrowTypeError(context, "UI node must be an object");
      }
      UiNodeRecord record;
      std::string kind;
      std::string classes;
      std::string text_value;
      const bool valid =
          uintProperty(context, object, "id", record.id) &&
          uintProperty(context, object, "parent", record.parent, UINT32_MAX) &&
          uintProperty(context, object, "canvas", record.canvas) &&
          stringProperty(context, object, "kind", kind, true) &&
          stringProperty(context, object, "class", classes) &&
          stringProperty(context, object, "text", text_value);
      JS_FreeValue(context, object);
      if (!valid ||
          strings.size() + classes.size() + text_value.size() > 1024 * 1024)
        return JS_ThrowTypeError(context, "invalid UI node fields");
      if (kind == "container")
        record.kind = static_cast<uint8_t>(UiNodeKind::Container);
      else if (kind == "text")
        record.kind = static_cast<uint8_t>(UiNodeKind::Text);
      else if (kind == "canvas")
        record.kind = static_cast<uint8_t>(UiNodeKind::Canvas);
      else
        return JS_ThrowRangeError(context, "UI node kind is unsupported");
      record.class_offset = static_cast<uint32_t>(strings.size());
      record.class_length = static_cast<uint32_t>(classes.size());
      strings.insert(strings.end(), classes.begin(), classes.end());
      record.text_offset = static_cast<uint32_t>(strings.size());
      record.text_length = static_cast<uint32_t>(text_value.size());
      strings.insert(strings.end(), text_value.begin(), text_value.end());
      records.push_back(record);
    }
    if (!self->ui->submit(records.data(), records.size(),
                          strings.empty() ? nullptr : strings.data(),
                          strings.size()))
      return JS_ThrowInternalError(context, "%s",
                                   self->ui->lastError().c_str());
    return JS_UNDEFINED;
  }

  static JSValue jsUiClear(JSContext *context, JSValueConst, int,
                           JSValueConst *) {
    Impl *self = from(context);
    if (self && self->ui)
      self->ui->clear();
    return JS_UNDEFINED;
  }

  static bool byteView(JSContext *context, JSValueConst value,
                       const uint8_t *&bytes, size_t &size, JSValue &backing) {
    bytes = JS_GetArrayBuffer(context, &size, value);
    if (bytes)
      return true;
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
    return true;
  }

  static JSValue jsModulesEnumerate(JSContext *context, JSValueConst, int,
                                    JSValueConst *) {
    Impl *self = from(context);
    JSValue result = JS_NewArray(context);
    if (!self || !self->package_modules)
      return result;
    uint32_t index = 0;
    for (const ModuleInfo &module : self->package_modules->enumerateModules()) {
      JSValue value = JS_NewObject(context);
      JS_SetPropertyStr(context, value, "name",
                        JS_NewString(context, module.name.c_str()));
      JS_SetPropertyStr(
          context, value, "runtime",
          JS_NewString(context,
                       module.runtime == apps::AppRuntimeKind::JavaScript
                           ? "js"
                           : "wasm"));
      JS_SetPropertyUint32(context, result, index++, value);
    }
    return result;
  }

  static JSValue jsModulesInvoke(JSContext *context, JSValueConst, int argc,
                                 JSValueConst *argv) {
    Impl *self = from(context);
    size_t module_size = 0;
    size_t operation_size = 0;
    const char *module =
        argc > 0 ? JS_ToCStringLen(context, &module_size, argv[0]) : nullptr;
    const char *operation =
        argc > 1 ? JS_ToCStringLen(context, &operation_size, argv[1]) : nullptr;
    const uint8_t *request = nullptr;
    size_t request_size = 0;
    JSValue backing = JS_UNDEFINED;
    const bool valid =
        self && self->package_modules && module && operation && argc > 2 &&
        byteView(context, argv[2], request, request_size, backing);
    std::vector<uint8_t> response;
    const bool success =
        valid && self->package_modules->invokeModule(
                     std::string(module, module_size),
                     std::string(operation, operation_size),
                     request_size ? request : nullptr, request_size, response);
    if (module)
      JS_FreeCString(context, module);
    if (operation)
      JS_FreeCString(context, operation);
    JS_FreeValue(context, backing);
    if (!success)
      return JS_ThrowInternalError(
          context, "%s",
          valid ? self->package_modules->moduleError().c_str()
                : "invoke expects a declared module, operation, and byte view");
    return newUint8Array(context, response.data(), response.size());
  }

  static JSValue jsWasmModuleDeclared(JSContext *context, JSValueConst,
                                      int argc, JSValueConst *argv) {
    Impl *self = from(context);
    size_t size = 0;
    const char *name =
        argc ? JS_ToCStringLen(context, &size, argv[0]) : nullptr;
    bool found = false;
    if (self && self->package_modules && name) {
      for (const ModuleInfo &module :
           self->package_modules->enumerateModules()) {
        if (module.runtime == apps::AppRuntimeKind::WebAssembly &&
            module.name == std::string_view(name, size)) {
          found = true;
          break;
        }
      }
    }
    if (name)
      JS_FreeCString(context, name);
    return JS_NewBool(context, found);
  }

  static int initializeCanvasModule(JSContext *context, JSModuleDef *module) {
    const struct Entry {
      const char *name;
      JSCFunction *function;
      int arguments;
    } entries[] = {
        {"create", jsCanvasCreate, 1},
        {"configure", jsCanvasConfigure, 2},
        {"destroy", jsCanvasDestroy, 1},
        {"submit2d", jsCanvasSubmit2d, 2},
        {"textureSet", jsCanvasTextureSet, 4},
        {"textureFree", jsCanvasTextureFree, 2},
        {"submitMesh", jsCanvasSubmitMesh, 5},
    };
    for (const Entry &entry : entries) {
      if (JS_SetModuleExport(context, module, entry.name,
                             JS_NewCFunction(context, entry.function,
                                             entry.name, entry.arguments)) < 0)
        return -1;
    }
    return 0;
  }

  static int initializeGraphicsModule(JSContext *context, JSModuleDef *module) {
    const struct Entry {
      const char *name;
      JSCFunction *function;
      int arguments;
    } entries[] = {
        {"surfaceSize", jsGraphicsSurfaceSize, 0},
        {"pixelsPerPoint", jsGraphicsPixelsPerPoint, 0},
        {"surfaceFormat", jsGraphicsSurfaceFormat, 0},
        {"supportedTextureFormats", jsGraphicsSupportedTextureFormats, 0},
        {"graphicsLimits", jsGraphicsLimits, 0},
        {"textureSet", jsGraphicsTextureSet, 3},
        {"textureFree", jsGraphicsTextureFree, 1},
        {"submit", jsGraphicsSubmit, 4},
    };
    for (const Entry &entry : entries) {
      if (JS_SetModuleExport(context, module, entry.name,
                             JS_NewCFunction(context, entry.function,
                                             entry.name, entry.arguments)) < 0)
        return -1;
    }
    return 0;
  }

  static int initializeGlesModule(JSContext *context, JSModuleDef *module) {
    const struct Entry {
      const char *name;
      JSCFunction *function;
      int arguments;
    } entries[] = {
        {"capabilities", jsGlesCapabilities, 1},
        {"textureSet", jsGlesTextureSet, 4},
        {"textureFree", jsGlesTextureFree, 2},
        {"bufferSet", jsGlesBufferSet, 5},
        {"bufferWrite", jsGlesBufferWrite, 4},
        {"bufferFree", jsGlesBufferFree, 2},
        {"shaderSet", jsGlesShaderSet, 4},
        {"shaderFree", jsGlesShaderFree, 2},
        {"programSet", jsGlesProgramSet, 4},
        {"programFree", jsGlesProgramFree, 2},
        {"submit", jsGlesSubmit, 3},
    };
    for (const Entry &entry : entries) {
      if (JS_SetModuleExport(context, module, entry.name,
                             JS_NewCFunction(context, entry.function,
                                             entry.name, entry.arguments)) < 0)
        return -1;
    }
    if (JS_SetModuleExport(context, module, "attributeLocation",
                           JS_NewCFunctionMagic(context, jsGlesLocation,
                                                "attributeLocation", 3,
                                                JS_CFUNC_generic_magic, 0)) < 0)
      return -1;
    return JS_SetModuleExport(context, module, "uniformLocation",
                              JS_NewCFunctionMagic(context, jsGlesLocation,
                                                   "uniformLocation", 3,
                                                   JS_CFUNC_generic_magic, 1));
  }

  static int initializeUiModule(JSContext *context, JSModuleDef *module) {
    if (JS_SetModuleExport(context, module, "submit",
                           JS_NewCFunction(context, jsUiSubmit, "submit", 1)) <
        0)
      return -1;
    return JS_SetModuleExport(context, module, "clear",
                              JS_NewCFunction(context, jsUiClear, "clear", 0));
  }

  static int initializeModulesModule(JSContext *context, JSModuleDef *module) {
    if (JS_SetModuleExport(
            context, module, "enumerate",
            JS_NewCFunction(context, jsModulesEnumerate, "enumerate", 0)) < 0)
      return -1;
    return JS_SetModuleExport(
        context, module, "invoke",
        JS_NewCFunction(context, jsModulesInvoke, "invoke", 3));
  }

  JSModuleDef *loadBuiltinModule(const char *name) {
    JSModuleInitFunc *initializer = nullptr;
    constexpr const char *canvas_exports[] = {
        "create",     "configure",   "destroy",   "submit2d",
        "textureSet", "textureFree", "submitMesh"};
    constexpr const char *graphics_exports[] = {
        "surfaceSize",    "pixelsPerPoint",
        "surfaceFormat",  "supportedTextureFormats",
        "graphicsLimits", "textureSet",
        "textureFree",    "submit"};
    constexpr const char *gles_exports[] = {
        "capabilities", "textureSet",  "textureFree",       "bufferSet",
        "bufferWrite",  "bufferFree",  "shaderSet",         "shaderFree",
        "programSet",   "programFree", "attributeLocation", "uniformLocation",
        "submit"};
    constexpr const char *ui_exports[] = {"submit", "clear"};
    constexpr const char *module_exports[] = {"enumerate", "invoke"};
    const char *const *exports = nullptr;
    size_t export_count = 0;
    if (std::strcmp(name, "oos:runtime") == 0)
      return loadJsRuntimeModule(context);
    if (std::strcmp(name, "oos:canvas") == 0) {
      initializer = initializeCanvasModule;
      exports = canvas_exports;
      export_count = std::size(canvas_exports);
    } else if (std::strcmp(name, "oos:graphics") == 0) {
      initializer = initializeGraphicsModule;
      exports = graphics_exports;
      export_count = std::size(graphics_exports);
    } else if (std::strcmp(name, "oos:gles") == 0) {
      initializer = initializeGlesModule;
      exports = gles_exports;
      export_count = std::size(gles_exports);
    } else if (std::strcmp(name, "oos:solid-internal") == 0) {
      initializer = initializeUiModule;
      exports = ui_exports;
      export_count = std::size(ui_exports);
    } else if (std::strcmp(name, "oos:modules") == 0) {
      initializer = initializeModulesModule;
      exports = module_exports;
      export_count = std::size(module_exports);
    } else if (isJsPlatformServiceModule(name)) {
      return loadJsPlatformServiceModule(context, name);
    } else {
      JS_ThrowReferenceError(context, "unknown OOS module '%s'", name);
      return nullptr;
    }
    JSModuleDef *module = JS_NewCModule(context, name, initializer);
    if (!module)
      return nullptr;
    for (size_t index = 0; index < export_count; ++index) {
      if (JS_AddModuleExport(context, module, exports[index]) < 0)
        return nullptr;
    }
    return module;
  }

  bool installGlobals() {
    JSValue global = JS_GetGlobalObject(context);
    JSValue console = JS_NewObject(context);
    if (JS_IsException(global) || JS_IsException(console)) {
      JS_FreeValue(context, console);
      JS_FreeValue(context, global);
      return false;
    }
    const bool console_ready =
        JS_SetPropertyStr(context, console, "log",
                          JS_NewCFunctionMagic(context, jsLog, "log", 1,
                                               JS_CFUNC_generic_magic, 0)) >=
            0 &&
        JS_SetPropertyStr(context, console, "warn",
                          JS_NewCFunctionMagic(context, jsLog, "warn", 1,
                                               JS_CFUNC_generic_magic, 1)) >=
            0 &&
        JS_SetPropertyStr(context, console, "error",
                          JS_NewCFunctionMagic(context, jsLog, "error", 1,
                                               JS_CFUNC_generic_magic, 2)) >= 0;
    if (!console_ready) {
      JS_FreeValue(context, console);
      JS_FreeValue(context, global);
      return false;
    }
    const bool globals_ready =
        JS_SetPropertyStr(context, global, "console", console) >= 0 &&
        JS_SetPropertyStr(context, global, "__oosInvokeWasm",
                          JS_NewCFunction(context, jsModulesInvoke,
                                          "__oosInvokeWasm", 3)) >= 0 &&
        JS_SetPropertyStr(context, global, "__oosWasmModuleDeclared",
                          JS_NewCFunction(context, jsWasmModuleDeclared,
                                          "__oosWasmModuleDeclared", 1)) >= 0;
    if (globals_ready) {
      static constexpr char bootstrap[] = R"JS(
        (() => {
          class Module {
            constructor(name) {
              if (typeof name !== "string" || !__oosWasmModuleDeclared(name))
                throw new TypeError("WebAssembly.Module expects a declared Wasm module name");
              Object.defineProperty(this, "name", { value: name });
            }
          }
          class Instance {
            constructor(module, imports = {}) {
              if (!(module instanceof Module))
                throw new TypeError("WebAssembly.Instance expects WebAssembly.Module");
              if (imports == null || typeof imports !== "object")
                throw new TypeError("WebAssembly imports must be an object");
              this.exports = new Proxy(Object.create(null), {
                get: (_, operation) => {
                  if (typeof operation !== "string") return undefined;
                  return (request = new Uint8Array()) =>
                    __oosInvokeWasm(module.name, operation, request);
                }
              });
            }
          }
          globalThis.WebAssembly = Object.freeze({
            Module,
            Instance,
            validate: name => typeof name === "string" && __oosWasmModuleDeclared(name),
            compile: async name => new Module(name),
            instantiate: async (source, imports = {}) => {
              const module = source instanceof Module ? source : new Module(source);
              return { module, instance: new Instance(module, imports) };
            }
          });
        })();
      )JS";
      JSValue installed = JS_Eval(context, bootstrap, sizeof(bootstrap) - 1,
                                  "oos:webassembly", JS_EVAL_TYPE_GLOBAL);
      if (JS_IsException(installed)) {
        JS_FreeValue(context, installed);
        JS_FreeValue(context, global);
        return false;
      }
      JS_FreeValue(context, installed);
    }
    JS_FreeValue(context, global);
    return globals_ready;
  }

  bool callExport(const char *name, int argc, JSValueConst *arguments,
                  JSValue &result, bool required) {
    JSValue function = JS_GetPropertyStr(context, module_namespace, name);
    if (JS_IsException(function)) {
      captureException(std::string("resolve export ") + name);
      return false;
    }
    if (!JS_IsFunction(context, function)) {
      JS_FreeValue(context, function);
      if (!required) {
        result = JS_UNDEFINED;
        return true;
      }
      error =
          std::string("JavaScript entry must export function '") + name + "'";
      return false;
    }
    beginExecution();
    result = JS_Call(context, function, JS_UNDEFINED, argc, arguments);
    JS_FreeValue(context, function);
    const bool success =
        settle(result, std::string("JavaScript export ") + name);
    endExecution();
    return success;
  }

  bool load(const char *path) {
    shutdown();
    error.clear();
    if (!path || options.application_directory.empty() ||
        !canonicalDirectory(options.application_directory, application_root) ||
        !canonicalDirectory(options.module_directory, module_root) ||
        !regularCanonicalFile(path, entry_path) ||
        !pathUnder(entry_path, application_root)) {
      error = "JavaScript entry or package directories are invalid";
      return false;
    }
    application = std::make_unique<ApplicationContext>(device, options);
    if (!application || !application->initialize()) {
      error = application ? application->lastError()
                          : "allocate JavaScript application context failed";
      shutdown();
      return false;
    }
    package_modules = std::make_unique<PackageModules>(
        *application, options.module_directory, options.modules);
    scene = dynamic_cast<ApplicationScene *>(&graphics);
    if (!scene) {
      owned_scene =
          std::make_unique<ApplicationScene>(graphics, options.font_directory);
      scene = owned_scene.get();
    }
    if (scene)
      ui = std::make_unique<NativeUiEngine>(*scene);
    runtime = JS_NewRuntime();
    if (!runtime) {
      error = "create QuickJS runtime failed";
      shutdown();
      return false;
    }
    JS_SetRuntimeOpaque(runtime, this);
    JS_SetMemoryLimit(runtime, options.memory_limit);
    JS_SetMaxStackSize(runtime, options.stack_limit);
    JS_SetCanBlock(runtime, false);
    JS_SetInterruptHandler(runtime, interrupt, this);
    JS_SetModuleLoaderFunc(runtime, normalizeModule, loadModule, this);
    context = JS_NewContext(runtime);
    if (!context) {
      error = "create QuickJS context failed";
      shutdown();
      return false;
    }
    JS_SetContextOpaque(context, this);
    if (!installGlobals()) {
      captureException("install JavaScript globals");
      shutdown();
      return false;
    }
    std::string source;
    if (!readSource(entry_path.c_str(), source, error)) {
      shutdown();
      return false;
    }
    beginExecution();
    JSValue compiled =
        JS_Eval(context, source.data(), source.size(), entry_path.c_str(),
                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) {
      captureException("compile JavaScript entry");
      endExecution();
      shutdown(false);
      return false;
    }
    auto *module = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(compiled));
    if (!setImportMeta(module, true)) {
      JS_FreeValue(context, compiled);
      captureException("instantiate JavaScript entry");
      endExecution();
      shutdown(false);
      return false;
    }
    JSValue evaluation = JS_EvalFunction(context, compiled);
    if (!settle(evaluation, "evaluate JavaScript entry")) {
      JS_FreeValue(context, evaluation);
      endExecution();
      shutdown(false);
      return false;
    }
    JS_FreeValue(context, evaluation);
    module_namespace = JS_GetModuleNamespace(context, module);
    if (JS_IsException(module_namespace)) {
      captureException("read JavaScript entry exports");
      endExecution();
      shutdown(false);
      return false;
    }
    endExecution();
    is_loaded = true;
    return true;
  }

  bool initialize() {
    if (!is_loaded) {
      error = "JavaScript app is not loaded";
      return false;
    }
    JSValue result = JS_UNDEFINED;
    if (!callExport("initialize", 0, nullptr, result, false))
      return false;
    const int success = JS_IsUndefined(result) ? 1 : JS_ToBool(context, result);
    JS_FreeValue(context, result);
    if (success <= 0) {
      if (success == 0)
        error = "JavaScript initialize returned false";
      else
        captureException("convert JavaScript initialize result");
      return false;
    }
    initialized = true;
    return true;
  }

  void shutdown(bool invoke_export = true) {
    if (context && initialized && invoke_export) {
      JSValue result = JS_UNDEFINED;
      callExport("shutdown", 0, nullptr, result, false);
      JS_FreeValue(context, result);
    }
    initialized = false;
    is_loaded = false;
    deadline_us = 0;
    if (context) {
      JS_FreeValue(context, module_namespace);
      module_namespace = JS_UNDEFINED;
      JS_FreeContext(context);
      context = nullptr;
    }
    if (runtime) {
      JS_FreeRuntime(runtime);
      runtime = nullptr;
    }
    package_modules.reset();
    if (application)
      application->closeSession();
    if (ui)
      ui->clear();
    ui.reset();
    if (scene)
      scene->reset();
    scene = nullptr;
    owned_scene.reset();
    application.reset();
    application_root.clear();
    module_root.clear();
    entry_path.clear();
    exit_requested = false;
  }

  GraphicsHost &graphics;
  device::Device *device = nullptr;
  JsAppOptions options;
  std::unique_ptr<ApplicationContext> application;
  std::unique_ptr<PackageModules> package_modules;
  std::unique_ptr<ApplicationScene> owned_scene;
  ApplicationScene *scene = nullptr;
  std::unique_ptr<NativeUiEngine> ui;
  JSRuntime *runtime = nullptr;
  JSContext *context = nullptr;
  JSValue module_namespace = JS_UNDEFINED;
  std::string application_root;
  std::string module_root;
  std::string entry_path;
  std::string error;
  uint64_t deadline_us = 0;
  bool is_loaded = false;
  bool initialized = false;
  bool exit_requested = false;
  bool audio_focused = true;
};

class JsPackageModuleEngine final : public JsPlatformHost, public ModuleEngine {
public:
  JsPackageModuleEngine(ApplicationContext &application, ModuleHost &modules,
                        std::string directory,
                        std::vector<apps::AppModule> declarations)
      : application(application), modules(modules),
        declarations(std::move(declarations)) {
    if (!canonicalDirectory(directory, module_root)) {
      error = "JavaScript package module directory is invalid";
      return;
    }
    runtime = JS_NewRuntime();
    if (!runtime) {
      error = "create JavaScript package module runtime failed";
      return;
    }
    JS_SetMemoryLimit(runtime, 16 * 1024 * 1024);
    JS_SetMaxStackSize(runtime, 512 * 1024);
    JS_SetCanBlock(runtime, false);
    JS_SetInterruptHandler(runtime, interrupt, this);
    JS_SetModuleLoaderFunc(runtime, normalizeModule, loadModule, this);
    context = JS_NewContext(runtime);
    if (!context) {
      error = "create JavaScript package module context failed";
      return;
    }
    JS_SetContextOpaque(context, this);
    JSValue global = JS_GetGlobalObject(context);
    JSValue console = JS_NewObject(context);
    JS_SetPropertyStr(context, console, "log",
                      JS_NewCFunction(context, log, "log", 1));
    JS_SetPropertyStr(context, global, "console", console);
    JS_FreeValue(context, global);
  }

  ~JsPackageModuleEngine() override {
    if (context) {
      for (auto &entry : namespaces)
        JS_FreeValue(context, entry.second);
      JS_FreeContext(context);
    }
    if (runtime)
      JS_FreeRuntime(runtime);
  }

  bool invoke(const apps::AppModule &module, const std::string &operation,
              const uint8_t *request, size_t request_size,
              std::vector<uint8_t> &response) override {
    error.clear();
    if (!context) {
      if (error.empty())
        error = "JavaScript package module runtime is unavailable";
      return false;
    }
    JSValue *module_namespace = load(module);
    if (!module_namespace)
      return false;
    JSValue function = JS_GetPropertyStr(context, *module_namespace, "invoke");
    if (!JS_IsFunction(context, function)) {
      JS_FreeValue(context, function);
      error = "JavaScript package module must export function 'invoke'";
      return false;
    }
    JSValue operation_value =
        JS_NewStringLen(context, operation.data(), operation.size());
    JSValue request_value = newUint8Array(context, request, request_size);
    if (JS_IsException(operation_value) || JS_IsException(request_value)) {
      JS_FreeValue(context, operation_value);
      JS_FreeValue(context, request_value);
      JS_FreeValue(context, function);
      capture("allocate JavaScript module arguments");
      return false;
    }
    JSValue arguments[2] = {operation_value, request_value};
    deadline_us = steadyMicros() + 50 * 1000;
    JSValue result = JS_Call(context, function, JS_UNDEFINED, 2, arguments);
    JS_FreeValue(context, operation_value);
    JS_FreeValue(context, request_value);
    JS_FreeValue(context, function);
    if (!settle(result, "invoke JavaScript package module")) {
      JS_FreeValue(context, result);
      deadline_us = 0;
      return false;
    }
    deadline_us = 0;
    size_t size = 0;
    uint8_t *bytes = JS_GetArrayBuffer(context, &size, result);
    JSValue backing = JS_UNDEFINED;
    if (!bytes) {
      size_t offset = 0;
      size_t bytes_per_element = 0;
      backing = JS_GetTypedArrayBuffer(context, result, &offset, &size,
                                       &bytes_per_element);
      size_t backing_size = 0;
      uint8_t *backing_bytes =
          JS_IsException(backing)
              ? nullptr
              : JS_GetArrayBuffer(context, &backing_size, backing);
      if (backing_bytes && offset <= backing_size &&
          size <= backing_size - offset)
        bytes = backing_bytes + offset;
    }
    const bool valid = bytes && size <= 1024 * 1024;
    if (valid)
      response.assign(bytes, bytes + size);
    else
      error = "JavaScript package module must return at most 1 MiB of bytes";
    JS_FreeValue(context, backing);
    JS_FreeValue(context, result);
    return valid;
  }

  const std::string &lastError() const override { return error; }

  ApplicationContext *jsApplicationContext() override { return &application; }

  bool jsRequestExit() override { return false; }

private:
  static JsPackageModuleEngine *from(JSContext *context) {
    return static_cast<JsPackageModuleEngine *>(JS_GetContextOpaque(context));
  }

  static int interrupt(JSRuntime *, void *opaque) {
    auto *self = static_cast<JsPackageModuleEngine *>(opaque);
    return self && self->deadline_us && steadyMicros() >= self->deadline_us;
  }

  void capture(std::string_view action) {
    JSValue exception = JS_GetException(context);
    const char *message = JS_ToCString(context, exception);
    error = std::string(action) + ": " +
            (message ? message : "JavaScript exception");
    if (message)
      JS_FreeCString(context, message);
    JS_FreeValue(context, exception);
  }

  bool settle(JSValue &value, std::string_view action) {
    if (JS_IsException(value)) {
      capture(action);
      return false;
    }
    for (uint32_t jobs = 0;; ++jobs) {
      JSContext *job_context = nullptr;
      const int status = JS_ExecutePendingJob(runtime, &job_context);
      if (status == 0)
        break;
      if (status < 0 || jobs >= kMaximumPendingJobs ||
          (deadline_us && steadyMicros() >= deadline_us)) {
        if (status < 0)
          capture(action);
        else
          error = std::string(action) + " exceeded its execution budget";
        return false;
      }
    }
    if (JS_PromiseState(context, value) == JS_PROMISE_REJECTED) {
      JSValue reason = JS_PromiseResult(context, value);
      JS_FreeValue(context, value);
      value = JS_Throw(context, reason);
      capture(action);
      return false;
    }
    if (JS_PromiseState(context, value) == JS_PROMISE_PENDING) {
      error = std::string(action) + " returned an unprogressable promise";
      return false;
    }
    return true;
  }

  const apps::AppModule *declared(std::string_view name) const {
    for (const auto &module : declarations) {
      if (module.name == name &&
          module.runtime == apps::AppRuntimeKind::JavaScript)
        return &module;
    }
    return nullptr;
  }

  bool resolve(const char *base_name, const char *name, std::string &path) {
    if (!name || !name[0])
      return false;
    const std::string_view specifier(name);
    if (specifier == "oos:runtime" || specifier == "oos:modules" ||
        isJsPlatformServiceModule(name)) {
      path = name;
      return true;
    }
    std::string candidate;
    if (specifier[0] == '.') {
      if (!base_name || std::string_view(base_name).rfind(module_root, 0) != 0)
        return false;
      candidate = directoryName(base_name) + "/" + std::string(specifier);
    } else if (specifier[0] != '/') {
      const apps::AppModule *module = declared(specifier);
      if (!module)
        return false;
      constexpr std::string_view prefix = apps::kModulePrefix;
      candidate = module_root + "/" + module->path.substr(prefix.size());
    }
    const bool resolved = !candidate.empty() &&
                          regularCanonicalFile(candidate, path) &&
                          pathUnder(path, module_root);
    return resolved && ((path.size() >= 3 &&
                         path.compare(path.size() - 3, 3, ".js") == 0) ||
                        (path.size() >= 4 &&
                         path.compare(path.size() - 4, 4, ".mjs") == 0));
  }

  static char *normalizeModule(JSContext *context, const char *base_name,
                               const char *name, void *opaque) {
    auto *self = static_cast<JsPackageModuleEngine *>(opaque);
    std::string path;
    if (!self || !self->resolve(base_name, name, path)) {
      JS_ThrowReferenceError(context, "undeclared JavaScript package module");
      return nullptr;
    }
    return js_strdup(context, path.c_str());
  }

  static JSModuleDef *loadModule(JSContext *context, const char *name,
                                 void *opaque) {
    auto *self = static_cast<JsPackageModuleEngine *>(opaque);
    if (std::strcmp(name, "oos:runtime") == 0)
      return loadJsRuntimeModule(context);
    if (std::strcmp(name, "oos:modules") == 0)
      return self->loadModulesBuiltin();
    if (isJsPlatformServiceModule(name))
      return loadJsPlatformServiceModule(context, name);
    std::string source;
    if (!readSource(name, source, self->error)) {
      JS_ThrowReferenceError(context, "%s", self->error.c_str());
      return nullptr;
    }
    JSValue compiled = JS_Eval(context, source.data(), source.size(), name,
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled))
      return nullptr;
    JSModuleDef *module =
        static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(compiled));
    JS_FreeValue(context, compiled);
    return module;
  }

  static JSValue log(JSContext *context, JSValueConst, int argc,
                     JSValueConst *argv) {
    for (int index = 0; index < argc; ++index) {
      const char *value = JS_ToCString(context, argv[index]);
      std::fprintf(stderr, "%s%s", index ? " " : "[js-module] ",
                   value ? value : "<value>");
      if (value)
        JS_FreeCString(context, value);
    }
    std::fputc('\n', stderr);
    return JS_UNDEFINED;
  }

  static bool bytesFrom(JSContext *context, JSValueConst value,
                        const uint8_t *&bytes, size_t &size, JSValue &backing) {
    bytes = JS_GetArrayBuffer(context, &size, value);
    if (bytes)
      return true;
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
    return true;
  }

  static JSValue invokeModule(JSContext *context, JSValueConst, int argc,
                              JSValueConst *argv) {
    auto *self = from(context);
    size_t name_size = 0;
    size_t operation_size = 0;
    const char *name =
        argc > 0 ? JS_ToCStringLen(context, &name_size, argv[0]) : nullptr;
    const char *operation =
        argc > 1 ? JS_ToCStringLen(context, &operation_size, argv[1]) : nullptr;
    const uint8_t *request = nullptr;
    size_t request_size = 0;
    JSValue backing = JS_UNDEFINED;
    const bool valid =
        self && name && operation && argc > 2 &&
        bytesFrom(context, argv[2], request, request_size, backing);
    std::vector<uint8_t> response;
    const bool success =
        valid && self->modules.invokeModule(
                     std::string(name, name_size),
                     std::string(operation, operation_size),
                     request_size ? request : nullptr, request_size, response);
    if (name)
      JS_FreeCString(context, name);
    if (operation)
      JS_FreeCString(context, operation);
    JS_FreeValue(context, backing);
    if (!success)
      return JS_ThrowInternalError(context, "%s",
                                   self ? self->modules.moduleError().c_str()
                                        : "package module host is unavailable");
    return newUint8Array(context, response.data(), response.size());
  }

  static JSValue enumerateModules(JSContext *context, JSValueConst, int,
                                  JSValueConst *) {
    auto *self = from(context);
    JSValue result = JS_NewArray(context);
    if (!self)
      return result;
    uint32_t index = 0;
    for (const ModuleInfo &module : self->modules.enumerateModules()) {
      JSValue item = JS_NewObject(context);
      JS_SetPropertyStr(context, item, "name",
                        JS_NewString(context, module.name.c_str()));
      JS_SetPropertyStr(
          context, item, "runtime",
          JS_NewString(context,
                       module.runtime == apps::AppRuntimeKind::JavaScript
                           ? "js"
                           : "wasm"));
      JS_SetPropertyUint32(context, result, index++, item);
    }
    return result;
  }

  static int initializeModules(JSContext *context, JSModuleDef *module) {
    if (JS_SetModuleExport(
            context, module, "invoke",
            JS_NewCFunction(context, invokeModule, "invoke", 3)) < 0)
      return -1;
    return JS_SetModuleExport(
        context, module, "enumerate",
        JS_NewCFunction(context, enumerateModules, "enumerate", 0));
  }

  JSModuleDef *loadModulesBuiltin() {
    JSModuleDef *module =
        JS_NewCModule(context, "oos:modules", initializeModules);
    if (!module)
      return nullptr;
    if (JS_AddModuleExport(context, module, "invoke") < 0 ||
        JS_AddModuleExport(context, module, "enumerate") < 0)
      return nullptr;
    return module;
  }

  JSValue *load(const apps::AppModule &module) {
    const auto found = namespaces.find(module.name);
    if (found != namespaces.end())
      return &found->second;
    constexpr std::string_view prefix = apps::kModulePrefix;
    if (module.path.rfind(prefix, 0) != 0) {
      error = "JavaScript package module path is invalid";
      return nullptr;
    }
    std::string path;
    if (!regularCanonicalFile(
            module_root + "/" + module.path.substr(prefix.size()), path) ||
        !pathUnder(path, module_root)) {
      error = "JavaScript package module file is unavailable";
      return nullptr;
    }
    std::string source;
    if (!readSource(path.c_str(), source, error))
      return nullptr;
    deadline_us = steadyMicros() + 50 * 1000;
    JSValue compiled =
        JS_Eval(context, source.data(), source.size(), path.c_str(),
                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) {
      capture("compile JavaScript package module");
      deadline_us = 0;
      return nullptr;
    }
    JSModuleDef *definition =
        static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(compiled));
    JSValue evaluation = JS_EvalFunction(context, compiled);
    if (!settle(evaluation, "evaluate JavaScript package module")) {
      JS_FreeValue(context, evaluation);
      deadline_us = 0;
      return nullptr;
    }
    JS_FreeValue(context, evaluation);
    JSValue module_namespace = JS_GetModuleNamespace(context, definition);
    if (JS_IsException(module_namespace)) {
      capture("read JavaScript package module exports");
      deadline_us = 0;
      return nullptr;
    }
    deadline_us = 0;
    return &namespaces.emplace(module.name, module_namespace).first->second;
  }

  ApplicationContext &application;
  ModuleHost &modules;
  std::vector<apps::AppModule> declarations;
  std::string module_root;
  std::unordered_map<std::string, JSValue> namespaces;
  JSRuntime *runtime = nullptr;
  JSContext *context = nullptr;
  std::string error;
  uint64_t deadline_us = 0;
};

std::unique_ptr<ModuleEngine>
createJsModuleEngine(ApplicationContext &application, ModuleHost &modules,
                     std::string module_directory,
                     std::vector<apps::AppModule> declarations) {
  return std::make_unique<JsPackageModuleEngine>(application, modules,
                                                 std::move(module_directory),
                                                 std::move(declarations));
}

JsApp::JsApp(GraphicsHost &graphics, JsAppOptions options)
    : impl_(std::make_unique<Impl>(graphics, nullptr, std::move(options))) {}

JsApp::JsApp(GraphicsHost &graphics, device::Device &device,
             JsAppOptions options)
    : impl_(std::make_unique<Impl>(graphics, &device, std::move(options))) {}

JsApp::~JsApp() = default;

bool JsApp::load(const char *path) { return impl_->load(path); }

bool JsApp::initialize() { return impl_->initialize(); }

bool JsApp::dispatchKey(const input::KeyEvent &event, int64_t monotonic_us) {
  if (!impl_->initialized)
    return false;
  JSValue argument = JS_NewObject(impl_->context);
  if (JS_IsException(argument)) {
    impl_->captureException("allocate JavaScript key event");
    return false;
  }
  JS_SetPropertyStr(impl_->context, argument, "code",
                    JS_NewUint32(impl_->context, event.code));
  JS_SetPropertyStr(
      impl_->context, argument, "action",
      JS_NewString(impl_->context, input::keyActionName(event.action)));
  JS_SetPropertyStr(impl_->context, argument, "monotonicTimeUs",
                    JS_NewBigInt64(impl_->context, monotonic_us));
  JSValue result = JS_UNDEFINED;
  const bool called = impl_->callExport("onKey", 1, &argument, result, false);
  JS_FreeValue(impl_->context, argument);
  if (!called)
    return false;
  const int handled =
      JS_IsUndefined(result) ? 0 : JS_ToBool(impl_->context, result);
  JS_FreeValue(impl_->context, result);
  if (handled < 0) {
    impl_->captureException("convert JavaScript onKey result");
    return false;
  }
  return handled != 0;
}

bool JsApp::render(int64_t monotonic_us, uint32_t &next_delay_ms) {
  if (!impl_->initialized)
    return false;
  JSValue argument = JS_NewBigInt64(impl_->context, monotonic_us);
  JSValue result = JS_UNDEFINED;
  const bool called = impl_->callExport("frame", 1, &argument, result, true);
  JS_FreeValue(impl_->context, argument);
  if (!called)
    return false;
  uint32_t delay = 0;
  if (JS_ToUint32(impl_->context, &delay, result) < 0) {
    JS_FreeValue(impl_->context, result);
    impl_->captureException("convert JavaScript frame result");
    return false;
  }
  JS_FreeValue(impl_->context, result);
  next_delay_ms = delay;
  if (impl_->owned_scene && !impl_->owned_scene->present()) {
    impl_->error = impl_->owned_scene->lastError();
    return false;
  }
  return true;
}

bool JsApp::takeExitRequest() {
  const bool requested = impl_->exit_requested;
  impl_->exit_requested = false;
  return requested;
}

void JsApp::setAudioFocused(bool focused) {
  impl_->audio_focused = focused;
  if (impl_->application)
    impl_->application->setAudioFocused(focused);
}

void JsApp::shutdown() { impl_->shutdown(); }

const char *JsApp::lastError() const { return impl_->error.c_str(); }

bool JsApp::loaded() const { return impl_->is_loaded; }

} // namespace oos::runtime
