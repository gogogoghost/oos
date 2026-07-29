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
