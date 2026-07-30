#pragma once

#include "oos/apps/app_manifest.h"
#include "oos/apps/zip_archive.h"

#include <memory>
#include <string>

typedef struct _WebKitWebContext WebKitWebContext;
typedef struct _WebKitURISchemeRequest WebKitURISchemeRequest;

namespace oos::web {

struct ZipAppArchiveState;

class ZipAppSource {
public:
  ZipAppSource(std::string app_id, std::string package_path,
               std::string entrypoint, apps::PackageKind package_kind);

  bool initialize(WebKitWebContext *context);
  std::string uriFor(const std::string &path) const;
  const std::string &lastError() const { return error_; }

private:
  static void handleRequest(WebKitURISchemeRequest *request, void *data);
  void finishRequest(WebKitURISchemeRequest *request);

  std::string app_id_;
  std::string package_path_;
  std::string entrypoint_;
  apps::PackageKind package_kind_;
  std::string scheme_;
  std::string host_;
  std::shared_ptr<ZipAppArchiveState> archive_;
  std::string error_;
};

} // namespace oos::web
