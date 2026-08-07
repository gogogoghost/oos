#pragma once

#include "oos/apps/app_manifest.h"

#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace oos::runtime {

class ApplicationContext;

struct ModuleInfo {
  std::string name;
  apps::AppRuntimeKind runtime = apps::AppRuntimeKind::WebAssembly;
};

class ModuleHost {
public:
  virtual ~ModuleHost() = default;
  virtual const std::vector<ModuleInfo> &enumerateModules() const = 0;
  virtual bool invokeModule(const std::string &name,
                            const std::string &operation,
                            const uint8_t *request, size_t request_size,
                            std::vector<uint8_t> &response) = 0;
  virtual const std::string &moduleError() const = 0;
};

class ModuleEngine {
public:
  virtual ~ModuleEngine() = default;
  virtual bool invoke(const apps::AppModule &module,
                      const std::string &operation, const uint8_t *request,
                      size_t request_size, std::vector<uint8_t> &response) = 0;
  virtual const std::string &lastError() const = 0;
};

std::unique_ptr<ModuleEngine>
createWasmModuleEngine(ApplicationContext &application, ModuleHost &modules,
                       std::string module_directory);
std::unique_ptr<ModuleEngine>
createJsModuleEngine(ApplicationContext &application, ModuleHost &modules,
                     std::string module_directory,
                     std::vector<apps::AppModule> declarations);

class PackageModules final : public ModuleHost {
public:
  PackageModules(ApplicationContext &application, std::string module_directory,
                 std::vector<apps::AppModule> declarations);
  ~PackageModules() override;

  PackageModules(const PackageModules &) = delete;
  PackageModules &operator=(const PackageModules &) = delete;

  const std::vector<ModuleInfo> &enumerateModules() const override;
  bool invokeModule(const std::string &name, const std::string &operation,
                    const uint8_t *request, size_t request_size,
                    std::vector<uint8_t> &response) override;
  const std::string &moduleError() const override;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::runtime
