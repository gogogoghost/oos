#include "oos/web/zip_app_source.h"

#include <gio/gio.h>
#include <glib.h>
#include <wpe/webkit.h>

#include <cstring>
#include <utility>
#include <vector>

namespace oos::web {
namespace {

const char *contentType(const std::string &path) {
  const size_t dot = path.rfind('.');
  const std::string extension =
      dot == std::string::npos ? "" : path.substr(dot);
  if (extension == ".html" || extension == ".htm")
    return "text/html";
  if (extension == ".css")
    return "text/css";
  if (extension == ".js" || extension == ".mjs")
    return "text/javascript";
  if (extension == ".json" || extension == ".webapp" ||
      extension == ".webmanifest")
    return "application/json";
  if (extension == ".svg")
    return "image/svg+xml";
  if (extension == ".png")
    return "image/png";
  if (extension == ".jpg" || extension == ".jpeg")
    return "image/jpeg";
  if (extension == ".gif")
    return "image/gif";
  if (extension == ".woff")
    return "font/woff";
  if (extension == ".woff2")
    return "font/woff2";
  if (extension == ".wasm")
    return "application/wasm";
  return "application/octet-stream";
}

} // namespace

ZipAppSource::ZipAppSource(std::string app_id, std::string package_path)
    : app_id_(std::move(app_id)), package_path_(std::move(package_path)) {}

bool ZipAppSource::initialize(WebKitWebContext *context) {
  if (!context) {
    error_ = "WPE Web context is unavailable";
    return false;
  }
  if (!archive_.open(package_path_.c_str())) {
    error_ = archive_.lastError();
    return false;
  }
  webkit_web_context_register_uri_scheme(context, "oos-app", handleRequest,
                                         this, nullptr);
  WebKitSecurityManager *security =
      webkit_web_context_get_security_manager(context);
  webkit_security_manager_register_uri_scheme_as_local(security, "oos-app");
  webkit_security_manager_register_uri_scheme_as_secure(security, "oos-app");
  webkit_security_manager_register_uri_scheme_as_cors_enabled(security,
                                                              "oos-app");
  return true;
}

std::string ZipAppSource::uriFor(const std::string &entrypoint) const {
  return "oos-app://" + app_id_ + "/" + entrypoint;
}

void ZipAppSource::handleRequest(WebKitURISchemeRequest *request, void *data) {
  static_cast<ZipAppSource *>(data)->finishRequest(request);
}

void ZipAppSource::finishRequest(WebKitURISchemeRequest *request) {
  const char *request_path = webkit_uri_scheme_request_get_path(request);
  request_path = request_path ? request_path : "";
  while (*request_path == '/')
    ++request_path;
  gchar *unescaped = g_uri_unescape_string(request_path, nullptr);
  const std::string path = unescaped ? unescaped : "";
  g_free(unescaped);
  std::vector<uint8_t> bytes;
  if (!apps::validPackagePath(path) || !archive_.read(path.c_str(), bytes)) {
    GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                        "application resource was not found");
    webkit_uri_scheme_request_finish_error(request, error);
    g_error_free(error);
    return;
  }
  void *copy = g_malloc(bytes.size());
  if (!bytes.empty())
    std::memcpy(copy, bytes.data(), bytes.size());
  GInputStream *stream =
      g_memory_input_stream_new_from_data(copy, bytes.size(), g_free);
  webkit_uri_scheme_request_finish(
      request, stream, static_cast<gint64>(bytes.size()), contentType(path));
  g_object_unref(stream);
}

} // namespace oos::web
