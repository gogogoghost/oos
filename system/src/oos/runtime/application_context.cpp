#include "oos/runtime/application_context.h"

#include "oos/apps/app_repository.h"
#include "oos/device/device.h"
#include "oos/device/service_provider.h"
#include "oos/media/media_service.h"
#include "oos/resources/font_assets.h"
#include "oos/resources/package_assets.h"
#include "oos/services/system_service.h"
#include "oos/storage/app_storage.h"
#include "oos/storage/device_storage.h"
#include "oos/ui/system_ui_state.h"
#include "oos/ui/system_ui_settings.h"

#include <algorithm>
#include <new>

namespace oos::runtime {

ApplicationContext::ApplicationContext(device::Device *device,
                                       ApplicationContextOptions options)
    : device_(device), options_(std::move(options)) {
  if (device_) {
    const device::DeviceDescriptor &descriptor = device_->descriptor();
    if (options_.wasm_target.cpu_core.empty() && descriptor.cpu_core)
      options_.wasm_target.cpu_core = descriptor.cpu_core;
    if (options_.wasm_target.cpu_arch.empty() && descriptor.cpu_arch)
      options_.wasm_target.cpu_arch = descriptor.cpu_arch;
  }
  const apps::WasmTargetProfile defaults = apps::defaultWasmTargetProfile();
  if (options_.wasm_target.cpu_core.empty())
    options_.wasm_target.cpu_core = defaults.cpu_core;
  if (options_.wasm_target.cpu_arch.empty())
    options_.wasm_target.cpu_arch = defaults.cpu_arch;
  if (!options_.data_directory.empty())
    storage_ = std::make_unique<storage::AppStorage>(options_.data_directory);
  if (!options_.internal_media_directory.empty() &&
      !options_.removable_media_directory.empty()) {
    device_storage_ = std::make_unique<storage::DeviceStorageService>(
        options_.internal_media_directory, options_.removable_media_directory);
  }
  if (!options_.font_directory.empty()) {
    font_assets_ =
        std::make_unique<resources::FontAssetService>(options_.font_directory);
  }
  if (!options_.asset_directory.empty()) {
    assets_ = std::make_unique<resources::PackageAssetService>(
        options_.asset_directory);
  }
  if (!options_.system_data_root.empty() &&
      apps::hasDeviceServicePermission(options_.service_permission_mask,
                                       apps::DeviceServicePermission::System)) {
    system_services_ = std::make_unique<services::SystemServiceHub>(
        options_.system_data_root, options_.app_repository);
  }
}

ApplicationContext::~ApplicationContext() { closeSession(); }

bool ApplicationContext::initialize() {
  if (initialized_)
    return true;
  error_.clear();
  if ((!options_.wasm_target.cpu_core.empty() &&
       !apps::validWasmTargetName(options_.wasm_target.cpu_core)) ||
      (!options_.wasm_target.cpu_arch.empty() &&
       !apps::validWasmTargetName(options_.wasm_target.cpu_arch))) {
    error_ = "invalid Wasm CPU core or architecture name";
    return false;
  }
  if (storage_ && !storage_->initialize()) {
    error_ = "initialize app storage: " + storage_->lastError();
    return false;
  }
  if (system_services_ && !system_services_->initialize()) {
    error_ = "initialize system services: " + system_services_->lastError();
    return false;
  }
  initialized_ = true;
  return true;
}

void ApplicationContext::closeSession() {
  media_.reset();
  if (services_) {
    for (const std::string &name : wake_locks_)
      services_->releaseWakeLock(name);
    wake_locks_.clear();
    services_->closeAllPcm();
  }
  services_.reset();
  if (assets_)
    assets_->closeAll();
  if (storage_)
    storage_->closeSessionStatements();
}

bool ApplicationContext::acquireWakeLock(const std::string &name) {
  device::ServiceProvider *provider = services();
  if (!provider || !provider->acquireWakeLock(name))
    return false;
  wake_locks_.push_back(name);
  return true;
}

bool ApplicationContext::releaseWakeLock(const std::string &name) {
  device::ServiceProvider *provider = services();
  if (!provider || !provider->releaseWakeLock(name))
    return false;
  const auto found = std::find(wake_locks_.begin(), wake_locks_.end(), name);
  if (found != wake_locks_.end())
    wake_locks_.erase(found);
  return true;
}

bool ApplicationContext::permissionGranted(uint32_t required_permission) const {
  return !options_.enforce_service_permissions || required_permission == 0 ||
         (options_.service_permission_mask & required_permission) != 0;
}

device::ServiceProvider *ApplicationContext::services() {
  if (!device_)
    return nullptr;
  if (!services_) {
    try {
      services_ = std::make_unique<device::ServiceProvider>(*device_);
      services_->setAudioFocused(audio_focused_);
    } catch (const std::bad_alloc &) {
      error_ = "allocate device services failed";
      return nullptr;
    }
  }
  return services_.get();
}

media::MediaService *ApplicationContext::media() {
  device::ServiceProvider *provider = services();
  if (!provider)
    return nullptr;
  if (!media_) {
    try {
      media_ = std::make_unique<media::MediaService>(*provider,
                                                     options_.asset_directory);
      media_->setFocused(audio_focused_);
    } catch (const std::bad_alloc &) {
      error_ = "allocate media service failed";
      return nullptr;
    }
  }
  return media_.get();
}

storage::AppStorage *ApplicationContext::storage() const {
  return storage_.get();
}

storage::DeviceStorageService *ApplicationContext::deviceStorage() const {
  return device_storage_.get();
}

resources::FontAssetService *ApplicationContext::fontAssets() const {
  return font_assets_.get();
}

resources::PackageAssetService *ApplicationContext::assets() const {
  return assets_.get();
}

services::SystemServiceHub *ApplicationContext::systemServices() const {
  return system_services_.get();
}

ui::StatusBarAppearanceController *ApplicationContext::statusBar() const {
  return options_.status_bar;
}

ui::SystemUiState *ApplicationContext::systemUiState() const {
  return options_.system_ui_state;
}

ui::SystemUiSettings *ApplicationContext::systemUiSettings() const {
  return options_.system_ui_settings;
}

void ApplicationContext::setAudioFocused(bool focused) {
  audio_focused_ = focused;
  if (media_)
    media_->setFocused(focused);
  if (services_)
    services_->setAudioFocused(focused);
}

bool ApplicationContext::listApplications(
    std::vector<ApplicationInfo> &applications) {
  error_.clear();
  applications.clear();
  const uint32_t allowed =
      apps::permissionBit(apps::DeviceServicePermission::AppsLaunch) |
      apps::permissionBit(apps::DeviceServicePermission::AppsManagement);
  if (!permissionGranted(allowed)) {
    error_ = "application listing permission denied";
    return false;
  }
  if (!options_.app_repository) {
    error_ = "application repository is unavailable";
    return false;
  }
  std::vector<apps::AppRecord> records;
  if (!options_.app_repository->list(records)) {
    error_ = options_.app_repository->lastError();
    return false;
  }
  applications.reserve(records.size());
  for (const apps::AppRecord &record : records) {
    applications.push_back({record.manifest.id, record.manifest.name,
                            record.manifest.version,
                            record.manifest.entry.runtime, record.enabled});
  }
  return true;
}

bool ApplicationContext::requestApplicationLaunch(const std::string &app_id) {
  error_.clear();
  const uint32_t allowed =
      apps::permissionBit(apps::DeviceServicePermission::AppsLaunch) |
      apps::permissionBit(apps::DeviceServicePermission::AppsManagement);
  if (!permissionGranted(allowed)) {
    error_ = "application launch permission denied";
    return false;
  }
  apps::AppRecord record;
  if (app_id.empty() || !options_.app_repository ||
      !options_.app_repository->resolve(app_id.c_str(), record)) {
    error_ = options_.app_repository
                 ? options_.app_repository->lastError()
                 : "application repository is unavailable";
    return false;
  }
  if (!record.enabled) {
    error_ = "application is disabled: " + app_id;
    return false;
  }
  launch_request_ = app_id;
  return true;
}

bool ApplicationContext::requestApplicationUninstall(
    const std::string &app_id) {
  error_.clear();
  const uint32_t required =
      apps::permissionBit(apps::DeviceServicePermission::AppsManagement);
  if (!permissionGranted(required)) {
    error_ = "application management permission denied";
    return false;
  }
  if (app_id.empty() || app_id == options_.app_id) {
    error_ = "an application cannot uninstall itself";
    return false;
  }
  apps::AppRecord record;
  if (!options_.app_repository ||
      !options_.app_repository->resolve(app_id.c_str(), record)) {
    error_ = options_.app_repository
                 ? options_.app_repository->lastError()
                 : "application repository is unavailable";
    return false;
  }
  uninstall_request_ = app_id;
  return true;
}

std::string ApplicationContext::takeApplicationLaunchRequest() {
  std::string request = std::move(launch_request_);
  launch_request_.clear();
  return request;
}

std::string ApplicationContext::takeApplicationUninstallRequest() {
  std::string request = std::move(uninstall_request_);
  uninstall_request_.clear();
  return request;
}

} // namespace oos::runtime
