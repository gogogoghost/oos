#pragma once

#include "oos/apps/zip_archive.h"

#include <string>

typedef struct _WebKitWebContext WebKitWebContext;
typedef struct _WebKitURISchemeRequest WebKitURISchemeRequest;

namespace oos::web {

class ZipAppSource {
public:
  ZipAppSource(std::string app_id, std::string package_path);

  bool initialize(WebKitWebContext *context);
  std::string uriFor(const std::string &entrypoint) const;
  const std::string &lastError() const { return error_; }

private:
  static void handleRequest(WebKitURISchemeRequest *request, void *data);
  void finishRequest(WebKitURISchemeRequest *request);

  std::string app_id_;
  std::string package_path_;
  apps::ZipArchive archive_;
  std::string error_;
};

} // namespace oos::web
