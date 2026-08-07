#include "oos/runtime/module_host.h"

#include "oos/runtime/application_context.h"

#include <algorithm>
#include <atomic>

namespace oos::runtime {
namespace {

constexpr size_t kMaximumModuleMessageBytes = 1024 * 1024;
constexpr uint32_t kMaximumModuleCallDepth = 8;

} // namespace

class PackageModules::Impl {
public:
  Impl(ApplicationContext &application, std::string directory,
       std::vector<apps::AppModule> next_declarations)
      : application(application), module_directory(std::move(directory)),
        declarations(std::move(next_declarations)) {
    infos.reserve(declarations.size());
    for (const apps::AppModule &module : declarations)
      infos.push_back({module.name, module.runtime});
  }

  const apps::AppModule *find(const std::string &name) const {
    const auto found = std::find_if(
        declarations.begin(), declarations.end(), [&](const auto &module) {
          return module.name == name;
        });
    return found == declarations.end() ? nullptr : &*found;
  }

  ApplicationContext &application;
  std::string module_directory;
  std::vector<apps::AppModule> declarations;
  std::vector<ModuleInfo> infos;
  std::unique_ptr<ModuleEngine> wasm;
  std::unique_ptr<ModuleEngine> js;
  std::string error;
  std::atomic<uint32_t> depth{0};
};

PackageModules::PackageModules(ApplicationContext &application,
                               std::string module_directory,
                               std::vector<apps::AppModule> declarations)
    : impl_(std::make_unique<Impl>(application, std::move(module_directory),
                                  std::move(declarations))) {}

PackageModules::~PackageModules() = default;

const std::vector<ModuleInfo> &PackageModules::enumerateModules() const {
  return impl_->infos;
}

bool PackageModules::invokeModule(const std::string &name,
                                  const std::string &operation,
                                  const uint8_t *request, size_t request_size,
                                  std::vector<uint8_t> &response) {
  impl_->error.clear();
  response.clear();
  if (name.empty() || operation.empty() || operation.size() > 256 ||
      (!request && request_size != 0) ||
      request_size > kMaximumModuleMessageBytes) {
    impl_->error = "module invocation arguments exceed the platform limit";
    return false;
  }
  const apps::AppModule *module = impl_->find(name);
  if (!module) {
    impl_->error = "module is not declared by the application manifest: " + name;
    return false;
  }
  const uint32_t previous_depth = impl_->depth.fetch_add(1);
  if (previous_depth >= kMaximumModuleCallDepth) {
    impl_->depth.fetch_sub(1);
    impl_->error = "module invocation depth exceeds the platform limit";
    return false;
  }
  struct DepthGuard {
    std::atomic<uint32_t> &depth;
    explicit DepthGuard(std::atomic<uint32_t> &value) : depth(value) {}
    ~DepthGuard() { depth.fetch_sub(1); }
  } guard(impl_->depth);

  ModuleEngine *engine = nullptr;
  if (module->runtime == apps::AppRuntimeKind::WebAssembly) {
    if (!impl_->wasm)
      impl_->wasm = createWasmModuleEngine(
          impl_->application, *this, impl_->module_directory);
    engine = impl_->wasm.get();
  } else {
    if (!impl_->js)
      impl_->js = createJsModuleEngine(impl_->application, *this,
                                       impl_->module_directory,
                                       impl_->declarations);
    engine = impl_->js.get();
  }
  if (!engine) {
    impl_->error = "allocate package module runtime failed";
    return false;
  }
  if (!engine->invoke(*module, operation, request, request_size, response)) {
    impl_->error = engine->lastError();
    return false;
  }
  if (response.size() > kMaximumModuleMessageBytes) {
    response.clear();
    impl_->error = "module response exceeds the platform limit";
    return false;
  }
  return true;
}

const std::string &PackageModules::moduleError() const { return impl_->error; }

} // namespace oos::runtime
