#pragma once

#include <string>

typedef struct _WebKitUserContentManager WebKitUserContentManager;
typedef struct _JSCValue JSCValue;
typedef struct _WebKitScriptMessageReply WebKitScriptMessageReply;

namespace oos::web {

class KaiOsApiBridge {
public:
  using CloseCallback = void (*)(void *context);

  KaiOsApiBridge(std::string app_id, std::string api_profile, int api_fd);
  ~KaiOsApiBridge();

  KaiOsApiBridge(const KaiOsApiBridge &) = delete;
  KaiOsApiBridge &operator=(const KaiOsApiBridge &) = delete;

  bool initialize(CloseCallback close_callback, void *close_context);
  WebKitUserContentManager *contentManager() const { return manager_; }
  const std::string &scriptSource() const { return script_; }
  const std::string &lastError() const { return error_; }

private:
  static void handleLifecycle(WebKitUserContentManager *manager,
                              JSCValue *message, void *data);
  static int handleDeviceApi(WebKitUserContentManager *manager,
                             JSCValue *message, WebKitScriptMessageReply *reply,
                             void *data);

  std::string app_id_;
  std::string api_profile_;
  int api_fd_ = -1;
  WebKitUserContentManager *manager_ = nullptr;
  CloseCallback close_callback_ = nullptr;
  void *close_context_ = nullptr;
  std::string script_;
  std::string error_;
};

} // namespace oos::web
