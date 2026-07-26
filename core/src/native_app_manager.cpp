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

  Impl(GraphicsHost &graphics, size_t resident_limit)
      : graphics(graphics), resident_limit(resident_limit) {}

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
  size_t resident_limit;
  std::vector<Entry> apps;
  size_t active_index = std::numeric_limits<size_t>::max();
  std::string error;
};

NativeAppManager::NativeAppManager(GraphicsHost &graphics,
                                   size_t resident_limit)
    : impl_(std::make_unique<Impl>(graphics, resident_limit)) {}

NativeAppManager::~NativeAppManager() = default;

bool NativeAppManager::load(const char *id, const char *module_path) {
  impl_->error.clear();
  if (!id || id[0] == '\0' || !module_path || module_path[0] == '\0') {
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

  auto app = std::make_unique<WasmApp>(impl_->graphics);
  if (!app->load(module_path) || !app->initialize()) {
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
