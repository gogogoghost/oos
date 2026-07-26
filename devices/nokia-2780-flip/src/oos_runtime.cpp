#include <binder/ProcessState.h>

#include <glib-unix.h>
#include <glib.h>
#include <png.h>
#include <wpe-android/view-backend.h>
#include <wpe/webkit.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "oos/input/key_input.h"
#include "oos/nokia2780/wpe_display_manager.h"

namespace oos::platform {
namespace {

using oos::input::KeyEvent;
using oos::input::KeyInput;
using oos::input::KeyInputOptions;
using oos::nokia2780::WpeDisplayManager;

constexpr const char *kDefaultBootSplash = "/opt/oos/share/oos/boot-splash.png";
constexpr const char *kDefaultLauncherPath =
    "/opt/oos/share/oos/launcher/index.html";
constexpr const char *kDefaultLauncherBaseUri =
    "file:///opt/oos/share/oos/launcher/";

struct CommitContext {
  WpeDisplayManager *display;
  WPEAndroidViewBackend *backend;
};

struct RuntimeContext {
  KeyInput *input;
  WebKitWebView *view;
  GMainLoop *loop;
  bool page_ready = false;
};

const char *environmentOr(const char *name, const char *fallback) {
  const char *value = std::getenv(name);
  return value && value[0] != '\0' ? value : fallback;
}

bool loadBootSplash(const char *path, std::vector<uint16_t> &rgb565) {
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&image, path)) {
    std::fprintf(stderr, "failed to open boot splash %s: %s\n", path,
                 image.message);
    return false;
  }
  if (image.width != WpeDisplayManager::kPrimaryWidth ||
      image.height != WpeDisplayManager::kPrimaryHeight) {
    std::fprintf(stderr, "boot splash has invalid size %ux%u\n", image.width,
                 image.height);
    png_image_free(&image);
    return false;
  }
  image.format = PNG_FORMAT_RGB;
  std::vector<uint8_t> rgb(PNG_IMAGE_SIZE(image));
  if (!png_image_finish_read(&image, nullptr, rgb.data(), 0, nullptr)) {
    std::fprintf(stderr, "failed to decode boot splash %s: %s\n", path,
                 image.message);
    png_image_free(&image);
    return false;
  }
  png_image_free(&image);

  rgb565.resize(static_cast<size_t>(WpeDisplayManager::kPrimaryWidth) *
                WpeDisplayManager::kPrimaryHeight);
  for (size_t index = 0; index < rgb565.size(); ++index) {
    const uint8_t red = rgb[index * 3];
    const uint8_t green = rgb[index * 3 + 1];
    const uint8_t blue = rgb[index * 3 + 2];
    rgb565[index] = static_cast<uint16_t>(((red & 0xf8) << 8) |
                                          ((green & 0xfc) << 3) | (blue >> 3));
  }
  return true;
}

bool loadLauncher(WebKitWebView *view) {
  const char *path = environmentOr("OOS_LAUNCHER_PATH", kDefaultLauncherPath);
  gchar *html = nullptr;
  gsize html_size = 0;
  GError *error = nullptr;
  if (!g_file_get_contents(path, &html, &html_size, &error)) {
    std::fprintf(stderr, "failed to read launcher %s: %s\n", path,
                 error ? error->message : "unknown error");
    if (error)
      g_error_free(error);
    return false;
  }

  webkit_web_view_load_html(
      view, html,
      environmentOr("OOS_LAUNCHER_BASE_URI", kDefaultLauncherBaseUri));
  g_free(html);
  return true;
}

void commitBuffer(void *context, WPEAndroidBuffer *buffer, int fence_fd) {
  auto *commit = static_cast<CommitContext *>(context);
  commit->display->present(commit->backend, buffer, fence_fd);
}

void destroyBackend(gpointer backend) {
  WPEAndroidViewBackend_destroy(static_cast<WPEAndroidViewBackend *>(backend));
}

gboolean refreshPrimary(gpointer display) {
  static_cast<WpeDisplayManager *>(display)->refresh();
  return G_SOURCE_CONTINUE;
}

void keyDispatchFinished(GObject *, GAsyncResult *result, gpointer data) {
  auto *view = static_cast<WebKitWebView *>(data);
  GError *error = nullptr;
  JSCValue *value =
      webkit_web_view_evaluate_javascript_finish(view, result, &error);
  if (value)
    g_object_unref(value);
  if (error) {
    std::fprintf(stderr, "launcher key dispatch failed: %s\n", error->message);
    g_error_free(error);
  }
}

void dispatchKey(void *data, const KeyEvent &event) {
  auto *context = static_cast<RuntimeContext *>(data);
  if (!context->page_ready)
    return;
  gchar *script =
      g_strdup_printf("window.oosHandleKey&&window.oosHandleKey(%u,'%s')",
                      event.code, oos::input::keyActionName(event.action));
  webkit_web_view_evaluate_javascript(context->view, script, -1, nullptr,
                                      nullptr, nullptr, keyDispatchFinished,
                                      context->view);
  g_free(script);
}

gboolean inputReady(gint, GIOCondition, gpointer data) {
  auto *context = static_cast<RuntimeContext *>(data);
  if (context->input->poll(0, dispatchKey, context) >= 0)
    return G_SOURCE_CONTINUE;
  std::fprintf(stderr, "OOS key input polling stopped\n");
  g_main_loop_quit(context->loop);
  return G_SOURCE_REMOVE;
}

void loadChanged(WebKitWebView *, WebKitLoadEvent event, gpointer data) {
  if (event != WEBKIT_LOAD_FINISHED)
    return;
  auto *context = static_cast<RuntimeContext *>(data);
  context->page_ready = true;
  std::fprintf(stderr, "OOS launcher page ready\n");
  std::fflush(stderr);
}

void launcherDiagnosticFinished(GObject *, GAsyncResult *result,
                                gpointer data) {
  auto *view = static_cast<WebKitWebView *>(data);
  GError *error = nullptr;
  JSCValue *value =
      webkit_web_view_evaluate_javascript_finish(view, result, &error);
  if (!value) {
    std::fprintf(stderr, "launcher DOM check failed: %s\n",
                 error ? error->message : "unknown evaluation error");
    if (error)
      g_error_free(error);
    return;
  }

  gchar *status = jsc_value_to_string(value);
  std::fprintf(stderr, "launcher DOM status: %s\n", status ? status : "(null)");
  std::fflush(stderr);
  g_free(status);
  g_object_unref(value);
}

gboolean reportLauncherStatus(gpointer data) {
  auto *view = static_cast<WebKitWebView *>(data);
  constexpr const char *script =
      "var root=document.getElementById('root');JSON.stringify({ready:"
      "document.readyState,rootChildren:root?root.childElementCount:-1,styles:"
      "document.styleSheets.length,background:getComputedStyle(document.body)."
      "backgroundColor})";
  webkit_web_view_evaluate_javascript(view, script, -1, nullptr, nullptr,
                                      nullptr, launcherDiagnosticFinished,
                                      view);
  return G_SOURCE_REMOVE;
}

gboolean loadFailed(WebKitWebView *, WebKitLoadEvent, const gchar *uri,
                    GError *error, gpointer) {
  std::fprintf(stderr, "launcher load failed for %s: %s\n",
               uri ? uri : "(unknown URI)",
               error ? error->message : "unknown error");
  std::fflush(stderr);
  return FALSE;
}

gboolean quitRuntime(gpointer data) {
  g_main_loop_quit(static_cast<GMainLoop *>(data));
  return G_SOURCE_REMOVE;
}

} // namespace

int run(int argc, char **argv) {
  if (argc > 1) {
    std::fprintf(stderr, "usage: %s\n", argv[0]);
    return 2;
  }

  android::ProcessState::self()->startThreadPool();
  WpeDisplayManager display(true);
  if (!display.initialize()) {
    std::fprintf(stderr, "failed to initialize Nokia 2780 display manager\n");
    return 1;
  }

  std::vector<uint16_t> boot_frame;
  const char *boot_path = environmentOr("OOS_BOOT_SPLASH", kDefaultBootSplash);
  if (!loadBootSplash(boot_path, boot_frame) ||
      !display.showBootFrame(boot_frame.data())) {
    std::fprintf(stderr, "failed to present OOS boot splash\n");
    return 1;
  }

  KeyInput input(KeyInputOptions{true});
  if (!input.initialize()) {
    std::fprintf(stderr, "failed to initialize OOS key input\n");
    return 1;
  }

  auto *backend = WPEAndroidViewBackend_create(
      WpeDisplayManager::kPrimaryWidth, WpeDisplayManager::kPrimaryHeight);
  if (!backend) {
    std::fprintf(stderr, "failed to create WPE Android backend\n");
    return 1;
  }
  CommitContext commit{&display, backend};
  WPEAndroidViewBackend_setCommitBufferHandler(backend, &commit, commitBuffer);
  auto *wrapped = webkit_web_view_backend_new(
      WPEAndroidViewBackend_getWPEViewBackend(backend), destroyBackend,
      backend);
  auto *view = webkit_web_view_new(wrapped);
  if (!view) {
    std::fprintf(stderr, "failed to create OOS WebKit view\n");
    destroyBackend(backend);
    return 1;
  }
  if (g_strcmp0(std::getenv("OOS_WEBKIT_CONSOLE"), "1") == 0) {
    webkit_settings_set_enable_write_console_messages_to_stdout(
        webkit_web_view_get_settings(view), TRUE);
  }

  GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
  RuntimeContext context{&input, view, loop};
  g_signal_connect(view, "load-changed", G_CALLBACK(loadChanged), &context);
  g_signal_connect(view, "load-failed", G_CALLBACK(loadFailed), nullptr);
  if (!loadLauncher(view)) {
    g_object_unref(view);
    g_main_loop_unref(loop);
    return 1;
  }

  const auto input_conditions =
      static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP);
  const guint input_source = g_unix_fd_add(
      input.fileDescriptor(), input_conditions, inputReady, &context);
  const guint diagnostic_source =
      g_timeout_add(1500, reportLauncherStatus, view);
  const guint refresh_source = g_timeout_add(500, refreshPrimary, &display);
  const guint sigint_source = g_unix_signal_add(SIGINT, quitRuntime, loop);
  const guint sigterm_source = g_unix_signal_add(SIGTERM, quitRuntime, loop);

  g_main_loop_run(loop);

  g_source_remove(sigterm_source);
  g_source_remove(sigint_source);
  g_source_remove(refresh_source);
  if (g_main_context_find_source_by_id(nullptr, diagnostic_source))
    g_source_remove(diagnostic_source);
  g_source_remove(input_source);
  g_object_unref(view);
  g_main_loop_unref(loop);
  return 0;
}

} // namespace oos::platform
