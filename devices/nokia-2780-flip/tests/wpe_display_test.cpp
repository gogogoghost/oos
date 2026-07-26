#include <binder/ProcessState.h>

#include <glib.h>
#include <jsc/jsc.h>
#include <wpe-android/view-backend.h>
#include <wpe/webkit.h>

#include <array>
#include <cstdio>
#include <cstring>

#include "cover_fixture.h"
#include "oos/nokia2780/display_control.h"
#include "oos/nokia2780/wpe_display_manager.h"

extern "C" {
typedef struct _WebKitWebViewBackend WebKitWebViewBackend;
WebKitWebViewBackend *webkit_web_view_backend_new(struct wpe_view_backend *,
                                                  GDestroyNotify, gpointer);
WebKitWebView *webkit_web_view_new(WebKitWebViewBackend *);
void webkit_web_view_load_html(WebKitWebView *, const gchar *, const gchar *);
}

namespace {

constexpr guint kDemoHoldMs = 3000;

using oos::nokia2780::WpeDisplayManager;

struct CommitContext {
  WpeDisplayManager *display;
  WPEAndroidViewBackend *backend;
};

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

void runtimeCheckFinished(GObject *, GAsyncResult *result, gpointer data) {
  auto *view = static_cast<WebKitWebView *>(data);
  GError *error = nullptr;
  JSCValue *value =
      webkit_web_view_evaluate_javascript_finish(view, result, &error);
  if (!value) {
    std::fprintf(stderr, "runtime check: FAIL (%s)\n",
                 error ? error->message : "unknown evaluation error");
    if (error)
      g_error_free(error);
    return;
  }

  gchar *status = jsc_value_to_string(value);
  std::fprintf(stderr, "runtime check DOM status: %s\n",
               status ? status : "(null)");
  std::fflush(stderr);
  g_free(status);
  g_object_unref(value);
}

gboolean reportRuntimeCheck(gpointer data) {
  auto *view = static_cast<WebKitWebView *>(data);
  webkit_web_view_evaluate_javascript(
      view, "document.getElementById('status').textContent", -1, nullptr,
      nullptr, nullptr, runtimeCheckFinished, view);
  return G_SOURCE_REMOVE;
}

struct SwitchDemo {
  WpeDisplayManager *display;
  GMainLoop *loop;
  std::array<uint16_t, NOKIA_2780_COVER_WIDTH * NOKIA_2780_COVER_HEIGHT>
      cover_frame;
  unsigned int phase = 0;
  bool failed = false;
};

gboolean runSwitchDemoStep(gpointer data) {
  auto *demo = static_cast<SwitchDemo *>(data);
  if (demo->phase == 0 && !demo->display->frameReady())
    return G_SOURCE_CONTINUE;

  bool ok = false;
  switch (demo->phase) {
  case 0:
    std::fprintf(stderr, "single-process demo: cover 1/%u ms\n", kDemoHoldMs);
    ok = demo->display->showCover(demo->cover_frame.data());
    break;
  case 1:
    std::fprintf(stderr, "single-process demo: primary/%u ms\n", kDemoHoldMs);
    ok = demo->display->showPrimary();
    break;
  case 2:
    std::fprintf(stderr, "single-process demo: cover 2/%u ms\n", kDemoHoldMs);
    ok = demo->display->showCover(demo->cover_frame.data());
    break;
  default:
    demo->display->shutdownDisplays();
    std::fprintf(stderr, "single-process demo complete\n");
    std::fflush(stderr);
    g_main_loop_quit(demo->loop);
    return G_SOURCE_REMOVE;
  }

  if (!ok) {
    demo->failed = true;
    demo->display->shutdownDisplays();
    std::fprintf(stderr, "single-process demo failed in phase %u\n",
                 demo->phase);
    std::fflush(stderr);
    g_main_loop_quit(demo->loop);
    return G_SOURCE_REMOVE;
  }
  std::fflush(stderr);
  ++demo->phase;
  g_timeout_add(kDemoHoldMs, runSwitchDemoStep, demo);
  return G_SOURCE_REMOVE;
}

} // namespace

int main(int argc, char **argv) {
  const bool switch_demo =
      argc == 2 && std::strcmp(argv[1], "--switch-demo") == 0;
  if (argc > 2 || (argc == 2 && !switch_demo)) {
    std::fprintf(stderr, "usage: %s [--switch-demo]\n", argv[0]);
    return 2;
  }

  android::ProcessState::self()->startThreadPool();
  WpeDisplayManager display(!switch_demo);
  if (!display.initialize()) {
    std::fprintf(stderr, "failed to initialize Nokia 2780 display manager\n");
    return 1;
  }

  auto *backend = WPEAndroidViewBackend_create(
      WpeDisplayManager::kPrimaryWidth, WpeDisplayManager::kPrimaryHeight);
  if (!backend)
    return 1;
  CommitContext commit{&display, backend};
  WPEAndroidViewBackend_setCommitBufferHandler(backend, &commit, commitBuffer);
  auto *wrapped = webkit_web_view_backend_new(
      WPEAndroidViewBackend_getWPEViewBackend(backend), destroyBackend,
      backend);
  auto *view = webkit_web_view_new(wrapped);
  if (!view) {
    std::fprintf(stderr, "failed to create WebKit view\n");
    destroyBackend(backend);
    return 1;
  }

  gchar *html = nullptr;
  gsize html_size = 0;
  GError *error = nullptr;
  if (!g_file_get_contents("/data/local/tmp/oos-wpe/hello.html", &html,
                           &html_size, &error)) {
    std::fprintf(stderr, "failed to read hello.html: %s\n",
                 error ? error->message : "unknown error");
    if (error)
      g_error_free(error);
    destroyBackend(backend);
    return 1;
  }
  webkit_web_view_load_html(view, html, "file:///data/local/tmp/oos-wpe/");
  g_free(html);
  g_timeout_add(3000, reportRuntimeCheck, view);

  // The primary panel periodically recovers from ESD and loses its scanout
  // target. Re-submit the retained GPU buffer without CPU copying.
  g_timeout_add(500, refreshPrimary, &display);
  GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
  SwitchDemo demo{&display, loop};
  if (switch_demo) {
    nokia2780_make_secondary_frame(demo.cover_frame.data());
    g_timeout_add(50, runSwitchDemoStep, &demo);
  }
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
  return demo.failed ? 1 : 0;
}
