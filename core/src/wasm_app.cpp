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

#include "oos/runtime/graphics_host.h"

namespace oos::runtime {
namespace {

constexpr size_t kErrorBufferSize = 512;
constexpr uint32_t kMaxLogBytes = 4096;
constexpr size_t kMaxModuleBytes = 32 * 1024 * 1024;

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

GraphicsHost *graphicsFor(wasm_exec_env_t environment) {
  wasm_module_inst_t instance = wasm_runtime_get_module_inst(environment);
  return static_cast<GraphicsHost *>(wasm_runtime_get_custom_data(instance));
}

uint32_t nativeAbiVersion(wasm_exec_env_t) { return OOS_WASM_ABI_VERSION; }

uint32_t nativeSurfaceWidth(wasm_exec_env_t environment) {
  GraphicsHost *graphics = graphicsFor(environment);
  return graphics ? graphics->width() : 0;
}

uint32_t nativeSurfaceHeight(wasm_exec_env_t environment) {
  GraphicsHost *graphics = graphicsFor(environment);
  return graphics ? graphics->height() : 0;
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

int32_t nativeTextureSet(wasm_exec_env_t environment, uint32_t texture,
                         uint32_t x, uint32_t y, uint32_t width,
                         uint32_t height, uint32_t flags, uint32_t offset,
                         uint32_t length) {
  GraphicsHost *graphics = graphicsFor(environment);
  if (!graphics || texture == 0 || width == 0 || height == 0 ||
      width > OOS_GFX_MAX_TEXTURE_SIZE || height > OOS_GFX_MAX_TEXTURE_SIZE ||
      (flags & ~(OOS_TEXTURE_LINEAR | OOS_TEXTURE_REPLACE)) != 0 ||
      width > std::numeric_limits<uint32_t>::max() / height / 4 ||
      length != width * height * 4 || length > OOS_GFX_MAX_TEXTURE_BYTES) {
    return -1;
  }
  const uint8_t *rgba =
      appArray<uint8_t>(environment, offset, length, OOS_GFX_MAX_TEXTURE_BYTES);
  return rgba && graphics->setTexture(texture, x, y, width, height, flags, rgba,
                                      length)
             ? 0
             : -1;
}

int32_t nativeTextureFree(wasm_exec_env_t environment, uint32_t texture) {
  GraphicsHost *graphics = graphicsFor(environment);
  return graphics && graphics->freeTexture(texture) ? 0 : -1;
}

int32_t nativeSubmit(wasm_exec_env_t environment, uint32_t vertex_offset,
                     uint32_t vertex_count, uint32_t index_offset,
                     uint32_t index_count, uint32_t command_offset,
                     uint32_t command_count, uint32_t clear_rgba) {
  GraphicsHost *graphics = graphicsFor(environment);
  const OosGfxVertex *vertices = appArray<OosGfxVertex>(
      environment, vertex_offset, vertex_count, OOS_GFX_MAX_VERTICES);
  const uint16_t *indices = appArray<uint16_t>(
      environment, index_offset, index_count, OOS_GFX_MAX_INDICES);
  const OosGfxDrawCommand *commands = appArray<OosGfxDrawCommand>(
      environment, command_offset, command_count, OOS_GFX_MAX_DRAW_COMMANDS);
  if (!graphics || !vertices || !indices || !commands)
    return -1;
  return graphics->submit(vertex_count == 0 ? nullptr : vertices, vertex_count,
                          index_count == 0 ? nullptr : indices, index_count,
                          command_count == 0 ? nullptr : commands,
                          command_count, clear_rgba)
             ? 0
             : -1;
}

NativeSymbol kNativeSymbols[] = {
    {"oos_abi_version", reinterpret_cast<void *>(nativeAbiVersion), "()i",
     nullptr},
    {"oos_surface_width", reinterpret_cast<void *>(nativeSurfaceWidth), "()i",
     nullptr},
    {"oos_surface_height", reinterpret_cast<void *>(nativeSurfaceHeight), "()i",
     nullptr},
    {"oos_wall_clock_minutes", reinterpret_cast<void *>(nativeWallClockMinutes),
     "()i", nullptr},
    {"oos_log", reinterpret_cast<void *>(nativeLog), "(iii)", nullptr},
    {"oos_gfx_texture_set", reinterpret_cast<void *>(nativeTextureSet),
     "(iiiiiiii)i", nullptr},
    {"oos_gfx_texture_free", reinterpret_cast<void *>(nativeTextureFree),
     "(i)i", nullptr},
    {"oos_gfx_submit", reinterpret_cast<void *>(nativeSubmit), "(iiiiiii)i",
     nullptr},
};

uint32_t gRuntimeReferences = 0;

bool acquireRuntime(std::string &error) {
  if (gRuntimeReferences == 0) {
    RuntimeInitArgs arguments = {};
    arguments.mem_alloc_type = Alloc_With_System_Allocator;
    arguments.native_module_name = "oos";
    arguments.native_symbols = kNativeSymbols;
    arguments.n_native_symbols =
        static_cast<uint32_t>(std::size(kNativeSymbols));
    if (!wasm_runtime_full_init(&arguments)) {
      error = "WAMR initialization failed";
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

  bool setTexture(uint32_t texture, uint32_t x, uint32_t y, uint32_t width,
                  uint32_t height, uint32_t flags, const uint8_t *rgba,
                  size_t rgba_size) override {
    auto *found = findTexture(texture);
    const bool inserted = found == nullptr;
    if (inserted) {
      if (x != 0 || y != 0)
        return false;
      const uint32_t host_texture = nextTextureHandle();
      if (host_texture == 0)
        return false;
      textures_.emplace_back(texture, host_texture);
      found = &textures_.back();
    }
    if (host_.setTexture(found->second, x, y, width, height, flags, rgba,
                         rgba_size)) {
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
    std::vector<OosGfxDrawCommand> translated;
    translated.reserve(command_count);
    for (size_t index = 0; index < command_count; ++index) {
      const auto *texture = findTexture(commands[index].texture);
      if (!texture)
        return false;
      translated.push_back(commands[index]);
      translated.back().texture = texture->second;
    }
    return host_.submit(vertices, vertex_count, indices, index_count,
                        translated.empty() ? nullptr : translated.data(),
                        translated.size(), clear_rgba);
  }

  void reset() {
    for (const auto &texture : textures_)
      host_.freeTexture(texture.second);
    textures_.clear();
  }

private:
  std::pair<uint32_t, uint32_t> *findTexture(uint32_t guest) {
    for (auto &texture : textures_) {
      if (texture.first == guest)
        return &texture;
    }
    return nullptr;
  }

  static uint32_t nextTextureHandle() {
    static std::atomic<uint32_t> next{1};
    uint32_t handle = next.fetch_add(1, std::memory_order_relaxed);
    if (handle == 0)
      handle = next.fetch_add(1, std::memory_order_relaxed);
    return handle;
  }

  GraphicsHost &host_;
  std::vector<std::pair<uint32_t, uint32_t>> textures_;
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
  Impl(GraphicsHost &graphics, WasmAppOptions options)
      : graphics(graphics), options(options) {}

  ~Impl() { shutdown(); }

  bool initializeRuntime() {
    if (runtime_initialized)
      return true;
    if (!acquireRuntime(error))
      return false;
    runtime_initialized = true;
    return true;
  }

  bool call(const char *name, uint32_t argc, uint32_t *argv,
            bool require_zero_result = false) {
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
    if (require_zero_result && argv && argv[0] != 0) {
      std::array<char, 32> result{};
      std::snprintf(result.data(), result.size(), "%u", argv[0]);
      error = std::string(name) + " returned " + result.data();
      return false;
    }
    return true;
  }

  void shutdown() {
    if (instance && environment) {
      wasm_function_inst_t function =
          wasm_runtime_lookup_function(instance, "oos_app_shutdown");
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
    : impl_(std::make_unique<Impl>(graphics, options)) {}

WasmApp::~WasmApp() = default;

bool WasmApp::load(const char *path) {
  impl_->shutdown();
  impl_->error.clear();
  if (!path || path[0] == '\0') {
    impl_->error = "WASM app path is empty";
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
  wasm_runtime_set_custom_data(impl_->instance, &impl_->graphics);
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
  impl_->initialized = impl_->call("oos_app_init", 0, result, true);
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
  return impl_->call("oos_app_event", std::size(arguments), arguments);
}

bool WasmApp::render(int64_t monotonic_us) {
  if (!impl_->initialized)
    return false;
  const uint64_t timestamp = static_cast<uint64_t>(monotonic_us);
  uint32_t arguments[2] = {
      static_cast<uint32_t>(timestamp),
      static_cast<uint32_t>(timestamp >> 32),
  };
  return impl_->call("oos_app_frame", std::size(arguments), arguments, true);
}

void WasmApp::shutdown() { impl_->shutdown(); }

const char *WasmApp::lastError() const { return impl_->error.c_str(); }

bool WasmApp::loaded() const {
  return impl_->instance != nullptr && impl_->environment != nullptr;
}

} // namespace oos::runtime
