#pragma once

#include "oos/apps/app_manifest.h"

#include <memory>
#include <string>
#include <vector>

namespace oos::apps {

struct AppRecord {
  AppManifest manifest;
  std::string package_path;
  std::string package_digest;
  bool enabled = true;
};

struct AppLaunch {
  AppRecord app;
  std::string executable_path;
  std::string entrypoint;
  std::string data_directory;
  std::string cache_directory;
  std::string application_directory;
  std::string asset_directory;
  std::string module_directory;
};

struct AppInstallOptions {
  std::string app_id;
};

class AppRepository {
public:
  explicit AppRepository(std::string data_root = "/data");
  ~AppRepository();

  AppRepository(const AppRepository &) = delete;
  AppRepository &operator=(const AppRepository &) = delete;

  bool initialize();
  bool install(const char *package_path, AppRecord *installed = nullptr);
  bool install(const char *package_path, const AppInstallOptions &options,
               AppRecord *installed = nullptr);
  bool resolve(const char *app_id, AppRecord &record);
  bool prepareLaunch(const char *app_id, AppLaunch &launch);
  bool list(std::vector<AppRecord> &records);
  bool uninstall(const char *app_id);

  const std::string &dataRoot() const { return data_root_; }
  const std::string &lastError() const { return error_; }

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  std::string data_root_;
  std::string error_;
};

} // namespace oos::apps
