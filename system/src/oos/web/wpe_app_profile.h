#pragma once

#include <string>

typedef struct _WebKitNetworkSession WebKitNetworkSession;
typedef struct _WebKitWebView WebKitWebView;
typedef struct _WebKitWebViewBackend WebKitWebViewBackend;

namespace oos::web {

class WpeAppProfile {
public:
  WpeAppProfile(std::string app_id, std::string data_directory,
                std::string cache_directory);
  ~WpeAppProfile();

  bool initialize();
  WebKitWebView *createView(WebKitWebViewBackend *backend);

  const std::string &lastError() const { return error_; }

private:
  std::string app_id_;
  std::string data_directory_;
  std::string cache_directory_;
  WebKitNetworkSession *session_ = nullptr;
  std::string error_;
};

} // namespace oos::web
