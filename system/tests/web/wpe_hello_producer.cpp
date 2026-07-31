#include <glib.h>
#include <jsc/jsc.h>
#include <wpe/webkit.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "oos/compositor/surface_transport.h"
#include "oos/web/transport_surface_sink.h"
#include "oos/web/wpe_app_profile.h"
#include "oos/web/wpe_surface_host.h"

extern "C" {
typedef struct _WebKitWebViewBackend WebKitWebViewBackend;
WebKitWebViewBackend *webkit_web_view_backend_new(struct wpe_view_backend *,
                                                  GDestroyNotify, gpointer);
void webkit_web_view_load_html(WebKitWebView *, const gchar *, const gchar *);
}

namespace {

constexpr uint64_t kWebSurfaceId = 1;

const char *environmentOr(const char *name, const char *fallback) {
  const char *value = std::getenv(name);
  return value && value[0] ? value : fallback;
}

uint32_t environmentDimension(const char *name, uint32_t fallback) {
  const unsigned long value =
      std::strtoul(environmentOr(name, "0"), nullptr, 10);
  return value > 0 && value <= UINT32_MAX ? static_cast<uint32_t>(value)
                                          : fallback;
}

struct TestState {
  GMainLoop *loop = nullptr;
  WebKitWebView *view = nullptr;
  oos::web::WpeSurfaceHost *surface = nullptr;
  bool runtime_passed = false;
};

void runtimeCheckFinished(GObject *, GAsyncResult *result, gpointer data) {
  auto *state = static_cast<TestState *>(data);
  GError *error = nullptr;
  JSCValue *value =
      webkit_web_view_evaluate_javascript_finish(state->view, result, &error);
  if (!value) {
    std::fprintf(stderr, "runtime check: FAIL (%s)\n",
                 error ? error->message : "unknown evaluation error");
    if (error)
      g_error_free(error);
    return;
  }
  gchar *status = jsc_value_to_string(value);
  state->runtime_passed = status && std::strstr(status, "PASS");
  std::fprintf(stderr, "runtime check DOM status: %s\n",
               status ? status : "(null)");
  std::fflush(stderr);
  g_free(status);
  g_object_unref(value);
}

gboolean reportRuntimeCheck(gpointer data) {
  auto *state = static_cast<TestState *>(data);
  webkit_web_view_evaluate_javascript(
      state->view, "document.getElementById('status').textContent", -1, nullptr,
      nullptr, nullptr, runtimeCheckFinished, state);
  return G_SOURCE_REMOVE;
}

gboolean finishTest(gpointer data) {
  auto *state = static_cast<TestState *>(data);
  std::fprintf(
      stderr, "WPE producer submitted %llu frames to OOS host\n",
      static_cast<unsigned long long>(state->surface->presentedFrames()));
  std::fflush(stderr);
  g_main_loop_quit(state->loop);
  return G_SOURCE_REMOVE;
}

void keepBackendOwnedByProducer(gpointer) {}

} // namespace

int main() {
  const char *socket_path = environmentOr(
      "OOS_SURFACE_SOCKET", "/data/local/tmp/oos-wpe/wpe-surface.sock");
  const int socket_fd = oos_surface_transport_connect(socket_path, 5000);
  if (socket_fd < 0) {
    std::fprintf(stderr, "failed to connect to OOS compositor: %d (%s)\n",
                 socket_fd, std::strerror(-socket_fd));
    return 1;
  }
  oos::web::TransportSurfaceSink surface_sink(socket_fd);
  if (!surface_sink.initialize()) {
    std::fprintf(stderr, "failed to initialize surface transport\n");
    return 1;
  }
  const uint32_t width = environmentDimension("OOS_SURFACE_WIDTH", 240);
  const uint32_t height = environmentDimension("OOS_SURFACE_HEIGHT", 320);
  oos::web::WpeSurfaceHost surface(surface_sink, kWebSurfaceId, width, height);
  if (!surface.initialize()) {
    std::fprintf(stderr, "failed to initialize WPE surface producer\n");
    return 1;
  }

  auto *wrapped = webkit_web_view_backend_new(
      surface.viewBackend(), keepBackendOwnedByProducer, nullptr);
  const char *data_root = environmentOr("OOS_DATA_ROOT", "/data");
  const char *app_id =
      environmentOr("OOS_WEB_APP_ID", "org.orangeos.wpe-smoke");
  const std::string data_directory =
      std::string(data_root) + "/users/0/web/" + app_id + "/data";
  const std::string cache_directory =
      std::string(data_root) + "/cache/web/" + app_id + "/webkit-2.52";
  oos::web::WpeAppProfile profile(app_id, data_directory, cache_directory);
  if (!profile.initialize()) {
    std::fprintf(stderr, "failed to initialize WPE app profile: %s\n",
                 profile.lastError().c_str());
    return 1;
  }
  auto *view = profile.createView(wrapped);
  if (!view) {
    std::fprintf(stderr, "failed to create WPE WebKit view\n");
    return 1;
  }

  const char *html_path =
      environmentOr("OOS_WPE_HELLO_HTML", "/opt/oos/tests/wpe/hello.html");
  gchar *html = nullptr;
  gsize html_size = 0;
  GError *error = nullptr;
  if (!g_file_get_contents(html_path, &html, &html_size, &error)) {
    std::fprintf(stderr, "failed to read %s: %s\n", html_path,
                 error ? error->message : "unknown error");
    if (error)
      g_error_free(error);
    g_object_unref(view);
    return 1;
  }
  webkit_web_view_load_html(view, html, "file:///opt/oos/tests/wpe/");
  g_free(html);

  GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
  TestState state{loop, view, &surface};
  g_timeout_add(3000, reportRuntimeCheck, &state);
  const unsigned long hold_ms =
      std::strtoul(environmentOr("OOS_WPE_TEST_HOLD_MS", "10000"), nullptr, 10);
  g_timeout_add(static_cast<guint>(hold_ms), finishTest, &state);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);

  const bool success = state.runtime_passed && surface.presentedFrames() > 0;
  g_object_unref(view);
  return success ? 0 : 1;
}
