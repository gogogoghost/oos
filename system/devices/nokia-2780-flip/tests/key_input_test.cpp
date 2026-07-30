#include <glib-unix.h>
#include <glib.h>
#include <jsc/jsc.h>
#include <wpe/webkit.h>

#include <cstdio>

#include "oos/compositor/compositor.h"
#include "oos/device/device.h"
#include "oos/device/display.h"
#include "oos/input/key_input.h"
#include "oos/web/wpe_surface_host.h"

extern "C" {
typedef struct _WebKitWebViewBackend WebKitWebViewBackend;
WebKitWebViewBackend *webkit_web_view_backend_new(struct wpe_view_backend *,
                                                  GDestroyNotify, gpointer);
WebKitWebView *webkit_web_view_new(WebKitWebViewBackend *);
void webkit_web_view_load_html(WebKitWebView *, const gchar *, const gchar *);
}

namespace {

using oos::input::KeyEvent;
using oos::input::KeyInput;
struct InputTestContext {
  KeyInput *input;
  WebKitWebView *view;
  bool page_ready = false;
};

gboolean refreshPrimary(gpointer display) {
  static_cast<oos::device::Display *>(display)->refresh();
  return G_SOURCE_CONTINUE;
}

void keepBackendOwnedByHost(gpointer) {}

void keyUpdateFinished(GObject *, GAsyncResult *result, gpointer data) {
  auto *view = static_cast<WebKitWebView *>(data);
  GError *error = nullptr;
  JSCValue *value =
      webkit_web_view_evaluate_javascript_finish(view, result, &error);
  if (value)
    g_object_unref(value);
  if (error) {
    std::fprintf(stderr, "key DOM update failed: %s\n", error->message);
    g_error_free(error);
  }
}

void showKey(void *data, const KeyEvent &event) {
  auto *context = static_cast<InputTestContext *>(data);
  const char *key_name = oos::input::keyCodeName(event.code);
  const char *action_name = oos::input::keyActionName(event.action);
  std::fprintf(
      stderr, "key code=%u name=%s action=%s device=%.*s path=%.*s\n",
      event.code, key_name, action_name,
      static_cast<int>(event.device_name.size()), event.device_name.data(),
      static_cast<int>(event.device_path.size()), event.device_path.data());
  std::fflush(stderr);

  if (!context->page_ready)
    return;
  gchar *escaped_key = g_strescape(key_name, nullptr);
  gchar *escaped_action = g_strescape(action_name, nullptr);
  gchar *device = g_strndup(event.device_name.data(), event.device_name.size());
  gchar *path = g_strndup(event.device_path.data(), event.device_path.size());
  gchar *escaped_device = g_strescape(device, nullptr);
  gchar *escaped_path = g_strescape(path, nullptr);
  gchar *script = g_strdup_printf(
      "window.oosShowKey(%u,\"%s\",\"%s\",\"%s\",\"%s\")", event.code,
      escaped_key, escaped_action, escaped_device, escaped_path);
  webkit_web_view_evaluate_javascript(context->view, script, -1, nullptr,
                                      nullptr, nullptr, keyUpdateFinished,
                                      context->view);
  g_free(script);
  g_free(escaped_path);
  g_free(escaped_device);
  g_free(path);
  g_free(device);
  g_free(escaped_action);
  g_free(escaped_key);
}

gboolean inputReady(gint, GIOCondition, gpointer data) {
  auto *context = static_cast<InputTestContext *>(data);
  if (context->input->poll(0, showKey, context) < 0) {
    std::fprintf(stderr, "key input polling stopped\n");
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

void loadChanged(WebKitWebView *, WebKitLoadEvent event, gpointer data) {
  if (event != WEBKIT_LOAD_FINISHED)
    return;
  auto *context = static_cast<InputTestContext *>(data);
  context->page_ready = true;
  std::fprintf(stderr, "key input page ready\n");
  std::fflush(stderr);
}

} // namespace

int main() {
  KeyInput input;
  if (!input.initialize())
    return 1;
  for (const auto &device : input.devices()) {
    std::fprintf(stderr, "key input device path=%s name=%s\n",
                 device.path.c_str(), device.name.c_str());
  }

  auto device = oos::device::createDevice();
  oos::device::DeviceInitOptions options;
  options.key_input = false;
  options.grab_input = false;
  if (!device || !device->initialize(options)) {
    std::fprintf(stderr, "failed to initialize OOS device\n");
    return 1;
  }
  oos::device::Display &display = device->display();
  oos::compositor::Compositor compositor(display);
  oos::web::WpeSurfaceHost surface(compositor, 1, display.width(),
                                   display.height());
  if (!surface.initialize())
    return 1;
  auto *wrapped = webkit_web_view_backend_new(surface.viewBackend(),
                                              keepBackendOwnedByHost, nullptr);
  auto *view = webkit_web_view_new(wrapped);
  if (!view) {
    std::fprintf(stderr, "failed to create WebKit view\n");
    return 1;
  }

  gchar *html = nullptr;
  gsize html_size = 0;
  GError *error = nullptr;
  if (!g_file_get_contents("/data/local/tmp/oos-wpe/input-test.html", &html,
                           &html_size, &error)) {
    std::fprintf(stderr, "failed to read input-test.html: %s\n",
                 error ? error->message : "unknown error");
    if (error)
      g_error_free(error);
    return 1;
  }

  InputTestContext context{&input, view};
  g_signal_connect(view, "load-changed", G_CALLBACK(loadChanged), &context);
  webkit_web_view_load_html(view, html, "file:///data/local/tmp/oos-wpe/");
  g_free(html);

  const auto input_conditions =
      static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP);
  g_unix_fd_add(input.fileDescriptor(), input_conditions, inputReady, &context);
  g_timeout_add(500, refreshPrimary, &display);
  GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  return 0;
}
