#include "oos/runtime/native_app_manager.h"

#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "oos/runtime/graphics_host.h"
#include "oos/runtime/wasm_app.h"

namespace oos::runtime {

class NativeAppManager::Impl {
public:
  struct Entry {
    std::string id;
    std::unique_ptr<WasmApp> app;
  };

  Impl(GraphicsHost &graphics, device::Device *device, size_t resident_limit)
      : graphics(graphics), device(device), resident_limit(resident_limit) {}

  Entry *find(const char *id) {
    if (!id)
      return nullptr;
    for (auto &entry : apps) {
      if (entry.id == id)
        return &entry;
    }
    return nullptr;
  }

  GraphicsHost &graphics;
  device::Device *device;
  size_t resident_limit;
  std::vector<Entry> apps;
  size_t active_index = std::numeric_limits<size_t>::max();
  std::string error;
};

NativeAppManager::NativeAppManager(GraphicsHost &graphics,
                                   size_t resident_limit)
    : impl_(std::make_unique<Impl>(graphics, nullptr, resident_limit)) {}

NativeAppManager::NativeAppManager(GraphicsHost &graphics,
                                   device::Device &device,
                                   size_t resident_limit)
    : impl_(std::make_unique<Impl>(graphics, &device, resident_limit)) {}

NativeAppManager::~NativeAppManager() = default;

bool NativeAppManager::load(const char *id, const char *module_path) {
  NativeAppLaunchOptions options;
  options.module_path = module_path;
  return load(id, options);
}

bool NativeAppManager::load(const char *id,
                            const NativeAppLaunchOptions &options) {
  impl_->error.clear();
  if (!id || id[0] == '\0' || !options.module_path ||
      options.module_path[0] == '\0') {
    impl_->error = "native app id or module path is empty";
    return false;
  }
  if (impl_->resident_limit == 0 ||
      impl_->apps.size() >= impl_->resident_limit) {
    impl_->error = "native app resident limit reached";
    return false;
  }
  if (impl_->find(id)) {
    impl_->error = std::string("native app already loaded: ") + id;
    return false;
  }

  WasmAppOptions wasm_options;
  wasm_options.stack_size = options.stack_size;
  wasm_options.heap_size = options.heap_size;
  wasm_options.app_id = id;
  wasm_options.data_directory =
      options.data_directory ? options.data_directory : "";
  wasm_options.system_data_root =
      options.system_data_root ? options.system_data_root : "";
  wasm_options.app_repository = options.app_repository;
  wasm_options.internal_media_directory =
      options.internal_media_directory ? options.internal_media_directory : "";
  wasm_options.removable_media_directory =
      options.removable_media_directory ? options.removable_media_directory
                                        : "";
  wasm_options.font_directory =
      options.font_directory ? options.font_directory : "";
  wasm_options.service_permission_mask = options.service_permission_mask;
  wasm_options.enforce_service_permissions =
      options.enforce_service_permissions;
  auto app = impl_->device
                 ? std::make_unique<WasmApp>(impl_->graphics, *impl_->device,
                                             wasm_options)
                 : std::make_unique<WasmApp>(impl_->graphics, wasm_options);
  if (!app->load(options.module_path) || !app->initialize()) {
    impl_->error =
        std::string("load native app ") + id + ": " + app->lastError();
    return false;
  }
  impl_->apps.push_back(Impl::Entry{id, std::move(app)});
  return true;
}

bool NativeAppManager::activate(const char *id) {
  impl_->error.clear();
  auto *entry = impl_->find(id);
  if (!entry) {
    impl_->error =
        std::string("native app is not loaded: ") + (id ? id : "(null)");
    return false;
  }
  impl_->active_index = static_cast<size_t>(entry - impl_->apps.data());
  return true;
}

bool NativeAppManager::remove(const char *id) {
  impl_->error.clear();
  for (size_t index = 0; index < impl_->apps.size(); ++index) {
    auto entry = impl_->apps.begin() + index;
    if (entry->id != (id ? id : ""))
      continue;
    if (impl_->active_index == index)
      impl_->active_index = std::numeric_limits<size_t>::max();
    else if (impl_->active_index > index &&
             impl_->active_index != std::numeric_limits<size_t>::max())
      --impl_->active_index;
    entry->app->shutdown();
    impl_->apps.erase(entry);
    return true;
  }
  impl_->error =
      std::string("native app is not loaded: ") + (id ? id : "(null)");
  return false;
}

bool NativeAppManager::dispatchKey(const input::KeyEvent &event,
                                   int64_t monotonic_us) {
  if (impl_->active_index >= impl_->apps.size()) {
    impl_->error = "no active native app";
    return false;
  }
  auto &active = impl_->apps[impl_->active_index].app;
  if (!active->dispatchKey(event, monotonic_us)) {
    impl_->error = active->lastError();
    return false;
  }
  return true;
}

bool NativeAppManager::render(int64_t monotonic_us) {
  if (impl_->active_index >= impl_->apps.size()) {
    impl_->error = "no active native app";
    return false;
  }
  auto &active = impl_->apps[impl_->active_index].app;
  if (!active->render(monotonic_us)) {
    impl_->error = active->lastError();
    return false;
  }
  return true;
}

void NativeAppManager::shutdown() {
  impl_->active_index = std::numeric_limits<size_t>::max();
  for (auto &entry : impl_->apps)
    entry.app->shutdown();
  impl_->apps.clear();
}

size_t NativeAppManager::residentCount() const { return impl_->apps.size(); }

const char *NativeAppManager::activeId() const {
  return impl_->active_index < impl_->apps.size()
             ? impl_->apps[impl_->active_index].id.c_str()
             : nullptr;
}

const char *NativeAppManager::lastError() const { return impl_->error.c_str(); }

} // namespace oos::runtime
