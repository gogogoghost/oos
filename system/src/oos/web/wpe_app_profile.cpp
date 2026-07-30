#include "oos/web/wpe_app_profile.h"

#include <glib.h>
#include <wpe/webkit.h>

#include <utility>

namespace oos::web {

WpeAppProfile::WpeAppProfile(std::string app_id, std::string data_directory,
                             std::string cache_directory)
    : app_id_(std::move(app_id)), data_directory_(std::move(data_directory)),
      cache_directory_(std::move(cache_directory)) {}

WpeAppProfile::~WpeAppProfile() {
  if (session_)
    g_object_unref(session_);
}

bool WpeAppProfile::initialize() {
  error_.clear();
  if (app_id_.empty() || data_directory_.empty() || cache_directory_.empty()) {
    error_ = "WPE application profile paths are empty";
    return false;
  }
  if (g_mkdir_with_parents(data_directory_.c_str(), 0700) != 0 ||
      g_mkdir_with_parents(cache_directory_.c_str(), 0700) != 0) {
    error_ = "cannot create WPE application data directories";
    return false;
  }

  const char *extension_directory = g_getenv("OOS_WPE_WEB_EXTENSIONS");
  if (!extension_directory || !*extension_directory)
    extension_directory = "/opt/oos/lib/oos/web-process-extensions";
  if (!g_file_test(extension_directory, G_FILE_TEST_IS_DIR)) {
    error_ = std::string("WPE WebProcess extension directory is missing: ") +
             extension_directory;
    return false;
  }

  WebKitWebContext *context = webkit_web_context_get_default();
  webkit_web_context_set_web_process_extensions_directory(
      context, extension_directory);
  webkit_web_context_add_path_to_sandbox(context, extension_directory, TRUE);

  session_ = webkit_network_session_new(data_directory_.c_str(),
                                        cache_directory_.c_str());
  if (!session_) {
    error_ = "cannot create persistent WPE network session";
    return false;
  }
  webkit_network_session_set_persistent_credential_storage_enabled(session_,
                                                                   TRUE);
  return true;
}

WebKitWebView *
WpeAppProfile::createView(WebKitWebViewBackend *backend,
                          WebKitUserContentManager *content_manager) {
  if (!session_ || !backend) {
    error_ = "WPE application profile is not initialized";
    return nullptr;
  }
  if (content_manager) {
    return WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW, "backend", backend, "network-session", session_,
        "user-content-manager", content_manager, nullptr));
  }
  return WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", backend,
                                      "network-session", session_, nullptr));
}

} // namespace oos::web
