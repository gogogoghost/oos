#include "oos/web/zip_app_source.h"

#include <gio/gio.h>
#include <glib.h>
#include <wpe/webkit.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace oos::web {

struct ZipAppArchiveState {
  apps::ZipArchive archive;
  std::mutex mutex;
};

namespace {

constexpr size_t kMaximumResourceBytes = 64 * 1024 * 1024;

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char character) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  });
  return value;
}

bool localhostHost(const std::string &app_id, std::string &host) {
  if (app_id.empty())
    return false;
  std::string normalized;
  normalized.reserve(app_id.size() + sizeof(".localhost"));
  bool label_start = true;
  for (const unsigned char character : app_id) {
    char output = static_cast<char>(std::tolower(character));
    if (output == '_')
      output = '-';
    if ((!std::isalnum(static_cast<unsigned char>(output)) && output != '-' &&
         output != '.') ||
        (label_start && (output == '-' || output == '.')))
      return false;
    if (output == '.') {
      if (normalized.empty() || normalized.back() == '-' ||
          normalized.back() == '.')
        return false;
      label_start = true;
    } else {
      label_start = false;
    }
    normalized.push_back(output);
  }
  if (normalized.back() == '-' || normalized.back() == '.')
    return false;
  host = normalized + ".localhost";
  return true;
}

bool hexDigit(char value, uint8_t &result) {
  if (value >= '0' && value <= '9')
    result = static_cast<uint8_t>(value - '0');
  else if (value >= 'a' && value <= 'f')
    result = static_cast<uint8_t>(value - 'a' + 10);
  else if (value >= 'A' && value <= 'F')
    result = static_cast<uint8_t>(value - 'A' + 10);
  else
    return false;
  return true;
}

bool decodePath(const char *encoded, std::string &path) {
  if (!encoded || encoded[0] != '/')
    return false;
  path.clear();
  for (size_t index = 1; encoded[index]; ++index) {
    if (encoded[index] != '%') {
      path.push_back(encoded[index]);
      continue;
    }
    uint8_t high = 0;
    uint8_t low = 0;
    if (!encoded[index + 1] || !encoded[index + 2] ||
        !hexDigit(encoded[index + 1], high) ||
        !hexDigit(encoded[index + 2], low))
      return false;
    const char decoded = static_cast<char>((high << 4) | low);
    if (decoded == '\0' || decoded == '/' || decoded == '\\')
      return false;
    path.push_back(decoded);
    index += 2;
  }
  return true;
}

const char *contentType(const std::string &path) {
  const size_t dot = path.rfind('.');
  const std::string extension =
      dot == std::string::npos ? "" : lower(path.substr(dot));
  if (extension == ".html" || extension == ".htm")
    return "text/html; charset=utf-8";
  if (extension == ".xhtml")
    return "application/xhtml+xml";
  if (extension == ".css")
    return "text/css; charset=utf-8";
  if (extension == ".js" || extension == ".mjs")
    return "text/javascript; charset=utf-8";
  if (extension == ".json")
    return "application/json";
  if (extension == ".webmanifest")
    return "application/manifest+json";
  if (extension == ".webapp")
    return "application/x-web-app-manifest+json";
  if (extension == ".xml")
    return "application/xml";
  if (extension == ".txt")
    return "text/plain; charset=utf-8";
  if (extension == ".svg")
    return "image/svg+xml";
  if (extension == ".png")
    return "image/png";
  if (extension == ".jpg" || extension == ".jpeg")
    return "image/jpeg";
  if (extension == ".gif")
    return "image/gif";
  if (extension == ".webp")
    return "image/webp";
  if (extension == ".bmp")
    return "image/bmp";
  if (extension == ".ico")
    return "image/x-icon";
  if (extension == ".woff")
    return "font/woff";
  if (extension == ".woff2")
    return "font/woff2";
  if (extension == ".ttf")
    return "font/ttf";
  if (extension == ".otf")
    return "font/otf";
  if (extension == ".wasm")
    return "application/wasm";
  if (extension == ".mp3")
    return "audio/mpeg";
  if (extension == ".ogg")
    return "audio/ogg";
  if (extension == ".wav")
    return "audio/wav";
  if (extension == ".m4a")
    return "audio/mp4";
  if (extension == ".aac")
    return "audio/aac";
  if (extension == ".flac")
    return "audio/flac";
  if (extension == ".mp4" || extension == ".m4v")
    return "video/mp4";
  if (extension == ".webm")
    return "video/webm";
  if (extension == ".ogv")
    return "video/ogg";
  return "application/octet-stream";
}

void deleteResourceBytes(gpointer data) {
  delete static_cast<std::vector<uint8_t> *>(data);
}

void finishStatus(WebKitURISchemeRequest *request, guint status,
                  const char *reason) {
  GInputStream *stream = g_memory_input_stream_new();
  WebKitURISchemeResponse *response = webkit_uri_scheme_response_new(stream, 0);
  webkit_uri_scheme_response_set_status(response, status, reason);
  webkit_uri_scheme_response_set_content_type(response,
                                              "text/plain; charset=utf-8");
  webkit_uri_scheme_request_finish_with_response(request, response);
  g_object_unref(response);
  g_object_unref(stream);
}

void finishResource(WebKitURISchemeRequest *request, const std::string &path,
                    gsize resource_size, std::vector<uint8_t> *owned_bytes) {
  GInputStream *stream = nullptr;
  if (owned_bytes) {
    GBytes *resource =
        g_bytes_new_with_free_func(owned_bytes->data(), owned_bytes->size(),
                                   deleteResourceBytes, owned_bytes);
    stream = g_memory_input_stream_new_from_bytes(resource);
    g_bytes_unref(resource);
  } else {
    stream = g_memory_input_stream_new();
  }

  WebKitURISchemeResponse *response = webkit_uri_scheme_response_new(
      stream, owned_bytes ? static_cast<gint64>(resource_size) : 0);
  webkit_uri_scheme_response_set_content_type(response, contentType(path));
  SoupMessageHeaders *headers =
      soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
  soup_message_headers_set_content_length(headers, resource_size);
  soup_message_headers_append(headers, "Cache-Control",
                              "public, max-age=31536000, immutable");
  webkit_uri_scheme_response_set_http_headers(response, headers);
  soup_message_headers_unref(headers);
  webkit_uri_scheme_request_finish_with_response(request, response);
  g_object_unref(response);
  g_object_unref(stream);
}

struct ResourceLoad {
  ResourceLoad(std::shared_ptr<ZipAppArchiveState> archive_state,
               WebKitURISchemeRequest *scheme_request, std::string entry_path)
      : archive(std::move(archive_state)),
        request(WEBKIT_URI_SCHEME_REQUEST(g_object_ref(scheme_request))),
        path(std::move(entry_path)) {}

  ~ResourceLoad() { g_object_unref(request); }

  std::shared_ptr<ZipAppArchiveState> archive;
  WebKitURISchemeRequest *request;
  std::string path;
  std::vector<uint8_t> bytes;
  bool loaded = false;
};

void deleteResourceLoad(gpointer data) {
  delete static_cast<ResourceLoad *>(data);
}

void loadResource(GTask *task, gpointer, gpointer task_data, GCancellable *) {
  auto *load = static_cast<ResourceLoad *>(task_data);
  {
    std::lock_guard<std::mutex> lock(load->archive->mutex);
    load->loaded = load->archive->archive.read(load->path.c_str(), load->bytes,
                                               kMaximumResourceBytes);
  }
  g_task_return_boolean(task, TRUE);
}

void resourceLoaded(GObject *, GAsyncResult *result, gpointer) {
  GTask *task = G_TASK(result);
  auto *load = static_cast<ResourceLoad *>(g_task_get_task_data(task));
  g_task_propagate_boolean(task, nullptr);
  if (!load->loaded) {
    finishStatus(load->request, 404, "Not Found");
    return;
  }
  finishResource(load->request, load->path, load->bytes.size(),
                 new std::vector<uint8_t>(std::move(load->bytes)));
}

} // namespace

ZipAppSource::ZipAppSource(std::string app_id, std::string package_path,
                           std::string entrypoint,
                           apps::PackageKind package_kind)
    : app_id_(std::move(app_id)), package_path_(std::move(package_path)),
      entrypoint_(std::move(entrypoint)), package_kind_(package_kind) {}

bool ZipAppSource::initialize(WebKitWebContext *context) {
  error_.clear();
  if (!context) {
    error_ = "WPE Web context is unavailable";
    return false;
  }
  if (package_kind_ == apps::PackageKind::KaiOs2) {
    scheme_ = "app";
    host_ = lower(app_id_);
  } else if (package_kind_ == apps::PackageKind::KaiOs3) {
    scheme_ = "http";
    if (!localhostHost(app_id_, host_)) {
      error_ = "application id cannot form a .localhost origin";
      return false;
    }
  } else {
    error_ = "ZIP application source requires a KaiOS package";
    return false;
  }
  if (!apps::validPackagePath(entrypoint_)) {
    error_ = "application entrypoint is not a safe package path";
    return false;
  }
  archive_ = std::make_shared<ZipAppArchiveState>();
  if (!archive_->archive.open(package_path_.c_str())) {
    error_ = archive_->archive.lastError();
    return false;
  }
  if (!archive_->archive.find(entrypoint_.c_str())) {
    error_ = "application entrypoint was not found in the package";
    return false;
  }

  webkit_web_context_register_uri_scheme(context, scheme_.c_str(),
                                         handleRequest, this, nullptr);
  if (scheme_ == "app") {
    WebKitSecurityManager *security =
        webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_local(security, "app");
    webkit_security_manager_register_uri_scheme_as_secure(security, "app");
    webkit_security_manager_register_uri_scheme_as_cors_enabled(security,
                                                                "app");
  }
  return true;
}

std::string ZipAppSource::uriFor(const std::string &path) const {
  gchar *escaped_path = g_uri_escape_string(path.c_str(), "/", TRUE);
  const std::string uri =
      scheme_ + "://" + host_ + "/" + (escaped_path ? escaped_path : "");
  g_free(escaped_path);
  return uri;
}

void ZipAppSource::handleRequest(WebKitURISchemeRequest *request, void *data) {
  static_cast<ZipAppSource *>(data)->finishRequest(request);
}

void ZipAppSource::finishRequest(WebKitURISchemeRequest *request) {
  const char *method = webkit_uri_scheme_request_get_http_method(request);
  const bool head = method && std::strcmp(method, "HEAD") == 0;
  if (method && std::strcmp(method, "GET") != 0 && !head) {
    finishStatus(request, 405, "Method Not Allowed");
    return;
  }

  GError *uri_error = nullptr;
  GUri *uri = g_uri_parse(webkit_uri_scheme_request_get_uri(request),
                          G_URI_FLAGS_ENCODED_PATH, &uri_error);
  if (!uri) {
    if (uri_error)
      g_error_free(uri_error);
    finishStatus(request, 400, "Bad Request");
    return;
  }
  const char *request_scheme = g_uri_get_scheme(uri);
  const char *request_host = g_uri_get_host(uri);
  const bool correct_origin =
      request_scheme && request_host && scheme_ == lower(request_scheme) &&
      host_ == lower(request_host) && g_uri_get_port(uri) == -1 &&
      !g_uri_get_userinfo(uri);
  std::string path;
  const bool valid_encoding = decodePath(g_uri_get_path(uri), path);
  g_uri_unref(uri);
  if (path.empty())
    path = entrypoint_;
  if (!correct_origin || !valid_encoding || !apps::validPackagePath(path)) {
    finishStatus(request, correct_origin ? 400 : 421,
                 correct_origin ? "Bad Request" : "Misdirected Request");
    return;
  }

  const apps::ZipEntry *entry = archive_->archive.find(path.c_str());
  if (!entry) {
    finishStatus(request, 404, "Not Found");
    return;
  }
  if (entry->uncompressed_size > kMaximumResourceBytes) {
    finishStatus(request, 413, "Content Too Large");
    return;
  }

  if (head) {
    finishResource(request, path, entry->uncompressed_size, nullptr);
    return;
  }

  GTask *task = g_task_new(nullptr, nullptr, resourceLoaded, nullptr);
  g_task_set_task_data(task,
                       new ResourceLoad(archive_, request, std::move(path)),
                       deleteResourceLoad);
  g_task_run_in_thread(task, loadResource);
  g_object_unref(task);
}

} // namespace oos::web
