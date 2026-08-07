#pragma once

#include "oos/apps/permissions.h"
#include "oos/apps/wasm_artifact.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oos::apps {
class AppRepository;
}

namespace oos::device {
class Device;
class ServiceProvider;
} // namespace oos::device

namespace oos::media {
class MediaService;
}

namespace oos::resources {
class FontAssetService;
class PackageAssetService;
} // namespace oos::resources

namespace oos::services {
class SystemServiceHub;
}

namespace oos::storage {
class AppStorage;
class DeviceStorageService;
} // namespace oos::storage

namespace oos::ui {
class StatusBarAppearanceController;
}

namespace oos::runtime {

struct ApplicationContextOptions {
  std::string app_id;
  std::string data_directory;
  std::string system_data_root;
  apps::AppRepository *app_repository = nullptr;
  ui::StatusBarAppearanceController *status_bar = nullptr;
  std::string internal_media_directory = "/data/media/internal";
  std::string removable_media_directory = "/data/media/removable";
  std::string font_directory = "/opt/oos/share/fonts";
  std::string asset_directory;
  int wake_fd = -1;
  uint32_t service_permission_mask = 0;
  bool enforce_service_permissions = false;
  apps::WasmTargetProfile wasm_target;
};

// Runtime-neutral ownership boundary for every service made available to an
// application. VM adapters only translate values and never construct their own
// storage, device, media, or permission implementations.
class ApplicationContext {
public:
  ApplicationContext(device::Device *device, ApplicationContextOptions options);
  ~ApplicationContext();

  ApplicationContext(const ApplicationContext &) = delete;
  ApplicationContext &operator=(const ApplicationContext &) = delete;

  bool initialize();
  void closeSession();

  bool permissionGranted(uint32_t required_permission) const;
  device::Device *device() const { return device_; }
  device::ServiceProvider *services();
  media::MediaService *media();
  media::MediaService *activeMedia() const { return media_.get(); }
  storage::AppStorage *storage() const;
  storage::DeviceStorageService *deviceStorage() const;
  resources::FontAssetService *fontAssets() const;
  resources::PackageAssetService *assets() const;
  services::SystemServiceHub *systemServices() const;
  ui::StatusBarAppearanceController *statusBar() const;

  const std::string &appId() const { return options_.app_id; }
  const std::string &assetDirectory() const { return options_.asset_directory; }
  const apps::WasmTargetProfile &wasmTarget() const {
    return options_.wasm_target;
  }
  int wakeFd() const { return options_.wake_fd; }
  bool acquireWakeLock(const std::string &name);
  bool releaseWakeLock(const std::string &name);
  void setAudioFocused(bool focused);
  bool audioFocused() const { return audio_focused_; }
  const std::string &lastError() const { return error_; }

private:
  device::Device *device_ = nullptr;
  ApplicationContextOptions options_;
  std::unique_ptr<device::ServiceProvider> services_;
  std::unique_ptr<media::MediaService> media_;
  std::unique_ptr<storage::AppStorage> storage_;
  std::unique_ptr<storage::DeviceStorageService> device_storage_;
  std::unique_ptr<resources::FontAssetService> font_assets_;
  std::unique_ptr<resources::PackageAssetService> assets_;
  std::unique_ptr<services::SystemServiceHub> system_services_;
  std::string error_;
  std::vector<std::string> wake_locks_;
  bool initialized_ = false;
  bool audio_focused_ = true;
};

} // namespace oos::runtime
