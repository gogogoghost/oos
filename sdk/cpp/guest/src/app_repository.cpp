#include "oos/apps/app_repository.h"

#include "app.h"

#include <algorithm>

namespace oos::apps {

class AppRepository::Impl {};

AppRepository::AppRepository(std::string data_root)
    : impl_(std::make_unique<Impl>()), data_root_(std::move(data_root)) {}
AppRepository::~AppRepository() = default;

bool AppRepository::initialize() {
  error_.clear();
  return true;
}

bool AppRepository::install(const char *, AppRecord *) {
  error_ = "applications cannot install packages directly";
  return false;
}

bool AppRepository::install(const char *, const AppInstallOptions &,
                            AppRecord *) {
  error_ = "applications cannot install packages directly";
  return false;
}

bool AppRepository::list(std::vector<AppRecord> &records) {
  oos_platform_applications_list_application_info_t applications{};
  oos_platform_applications_error_code_t error{};
  if (!oos_platform_applications_enumerate(&applications, &error)) {
    error_ = "application enumeration failed";
    return false;
  }
  records.clear();
  records.reserve(applications.len);
  for (size_t index = 0; index < applications.len; ++index) {
    const auto &source = applications.ptr[index];
    AppRecord record;
    record.manifest.schema = 1;
    record.manifest.id.assign(
        reinterpret_cast<const char *>(source.id.ptr), source.id.len);
    record.manifest.name.assign(
        reinterpret_cast<const char *>(source.name.ptr), source.name.len);
    record.manifest.version.assign(
        reinterpret_cast<const char *>(source.version.ptr), source.version.len);
    record.manifest.entry.runtime =
        source.runtime == OOS_PLATFORM_APPLICATIONS_RUNTIME_KIND_JS
            ? AppRuntimeKind::JavaScript
            : AppRuntimeKind::WebAssembly;
    record.enabled = source.enabled;
    records.push_back(std::move(record));
  }
  oos_platform_applications_list_application_info_free(&applications);
  error_.clear();
  return true;
}

bool AppRepository::resolve(const char *app_id, AppRecord &record) {
  std::vector<AppRecord> records;
  if (!list(records))
    return false;
  const auto found = std::find_if(
      records.begin(), records.end(),
      [app_id](const AppRecord &candidate) {
        return candidate.manifest.id == (app_id ? app_id : "");
      });
  if (found == records.end()) {
    error_ = "application is not installed";
    return false;
  }
  record = *found;
  return true;
}

bool AppRepository::prepareLaunch(const char *, AppLaunch &) {
  error_ = "applications cannot resolve another application's executable";
  return false;
}

bool AppRepository::uninstall(const char *app_id) {
  const std::string value = app_id ? app_id : "";
  app_string_t id{reinterpret_cast<uint8_t *>(
                      const_cast<char *>(value.data())),
                  value.size()};
  oos_platform_applications_error_code_t error{};
  if (!oos_platform_applications_uninstall(&id, &error)) {
    error_ = "application uninstall request failed";
    return false;
  }
  error_.clear();
  return true;
}

} // namespace oos::apps
