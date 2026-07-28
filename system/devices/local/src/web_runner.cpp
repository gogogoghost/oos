#include "oos/apps/app_repository.h"
#include "oos/compositor/compositor.h"
#include "oos/device/device.h"
#include "oos/input/key_input.h"
#include "oos/web/wpe_app_profile.h"
#include "oos/web/wpe_surface_host.h"
#include "oos/web/zip_app_source.h"

#include <glib-unix.h>
#include <glib.h>
#include <wpe/webkit.h>
#include <wpe/wpe.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <signal.h>
#include <string>

extern "C" {
typedef struct _WebKitWebViewBackend WebKitWebViewBackend;
WebKitWebViewBackend *webkit_web_view_backend_new(wpe_view_backend *,
                                                  GDestroyNotify, gpointer);
}

namespace {

constexpr uint64_t kWebSurfaceId = 1;

uint32_t keySymbol(uint16_t code) {
  if (code >= 2 && code <= 10)
    return WPE_KEY_1 + (code - 2);
  switch (code) {
  case 11:
    return WPE_KEY_0;
  case 103:
    return WPE_KEY_Up;
  case 105:
    return WPE_KEY_Left;
  case 106:
    return WPE_KEY_Right;
  case 108:
    return WPE_KEY_Down;
  case 139:
    return WPE_KEY_Menu;
  case 158:
    return WPE_KEY_BackSpace;
  case 352:
    return WPE_KEY_Return;
  case 357:
    return WPE_KEY_Option;
  case 522:
    return WPE_KEY_asterisk;
  case 523:
    return WPE_KEY_numbersign;
  default:
    return 0;
  }
}

struct RunnerState {
  GMainLoop *loop = nullptr;
  oos::input::KeyInputSource *input = nullptr;
  wpe_view_backend *backend = nullptr;
};

void dispatchKey(void *data, const oos::input::KeyEvent &key) {
  auto *state = static_cast<RunnerState *>(data);
  const uint32_t symbol = keySymbol(key.code);
  if (!symbol)
    return;
  wpe_input_keyboard_event event = {
      static_cast<uint32_t>(key.timestamp_us / 1000), symbol, key.code,
      key.action != oos::input::KeyAction::Released, 0};
  wpe_view_backend_dispatch_keyboard_event(state->backend, &event);
}

gboolean pumpLocalEvents(gpointer data) {
  auto *state = static_cast<RunnerState *>(data);
  if (state->input->poll(0, dispatchKey, state) < 0 ||
      state->input->stopRequested()) {
    g_main_loop_quit(state->loop);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

gboolean stopRunner(gpointer data) {
  g_main_loop_quit(static_cast<GMainLoop *>(data));
  return G_SOURCE_REMOVE;
}

void keepBackendOwnedByRunner(gpointer) {}

char *appUri(const char *path) {
  if (std::strstr(path, "://"))
    return g_strdup(path);
  GError *error = nullptr;
  char *uri = g_filename_to_uri(path, nullptr, &error);
  if (!uri) {
    std::fprintf(stderr, "failed to create web app URI for %s: %s\n", path,
                 error ? error->message : "unknown error");
    if (error)
      g_error_free(error);
  }
  return uri;
}

} // namespace

int main(int argc, char **argv) {
  if (argc > 3 || (argc == 3 && std::strcmp(argv[1], "--app") != 0)) {
    std::fprintf(stderr, "usage: %s [APP.html|URI | --app APP_ID]\n", argv[0]);
    return 2;
  }
  const char *configured = std::getenv("OOS_WEB_APP");
  const char *path = argc == 2 ? argv[1]
                     : configured && configured[0]
                         ? configured
                         : "/opt/oos/apps/web-launcher/index.html";
  const char *data_root = std::getenv("OOS_DATA_ROOT");
  data_root = data_root && data_root[0] ? data_root : "/data";
  const char *configured_id = std::getenv("OOS_WEB_APP_ID");
  const char *app_id = argc == 3 ? argv[2]
                       : configured_id && configured_id[0]
                           ? configured_id
                           : "org.orangeos.web-local";
  oos::apps::AppLaunch registered_app;
  std::unique_ptr<oos::apps::AppRepository> repository;
  const bool use_registered_app =
      argc == 3 || (configured_id && configured_id[0]);
  if (use_registered_app) {
    repository = std::make_unique<oos::apps::AppRepository>(data_root);
    if (!repository->initialize() ||
        !repository->prepareLaunch(app_id, registered_app) ||
        registered_app.app.manifest.runtime_kind !=
            oos::apps::RuntimeKind::Wpe) {
      std::fprintf(stderr, "failed to prepare WPE application %s: %s\n", app_id,
                   repository->lastError().c_str());
      return 1;
    }
  }

  std::unique_ptr<oos::device::Device> device = oos::device::createDevice();
  if (!device || !device->initialize()) {
    std::fprintf(stderr, "failed to initialize local OOS device: %s\n",
                 device ? device->lastError().c_str() : "factory unavailable");
    return 1;
  }

  oos::compositor::Compositor compositor(device->display());
  oos::web::WpeSurfaceHost surface(compositor, kWebSurfaceId,
                                   compositor.width(), compositor.height());
  if (!surface.initialize()) {
    std::fprintf(stderr, "failed to initialize local WPE SHM backend\n");
    return 1;
  }

  WebKitWebViewBackend *wrapped = webkit_web_view_backend_new(
      surface.viewBackend(), keepBackendOwnedByRunner, nullptr);
  const std::string data_directory =
      use_registered_app
          ? registered_app.data_directory + "/data"
          : std::string(data_root) + "/users/0/web/" + app_id + "/data";
  const std::string cache_directory =
      use_registered_app
          ? registered_app.cache_directory
          : std::string(data_root) + "/cache/web/" + app_id + "/webkit-2.52";
  WebKitWebContext *web_context = webkit_web_context_get_default();
  std::unique_ptr<oos::web::ZipAppSource> zip_source;
  std::string registered_uri;
  if (use_registered_app) {
    zip_source = std::make_unique<oos::web::ZipAppSource>(
        app_id, registered_app.app.package_path);
    if (!zip_source->initialize(web_context)) {
      std::fprintf(stderr, "failed to open WPE ZIP application: %s\n",
                   zip_source->lastError().c_str());
      return 1;
    }
    registered_uri = zip_source->uriFor(registered_app.app.manifest.entrypoint);
    path = registered_uri.c_str();
  }
  oos::web::WpeAppProfile profile(app_id, data_directory, cache_directory);
  if (!profile.initialize()) {
    std::fprintf(stderr, "failed to initialize WPE app profile: %s\n",
                 profile.lastError().c_str());
    return 1;
  }
  WebKitWebView *view = profile.createView(wrapped);
  char *uri = appUri(path);
  if (!view || !uri) {
    if (view)
      g_object_unref(view);
    g_free(uri);
    return 1;
  }
  webkit_web_view_load_uri(view, uri);
  std::fprintf(stderr, "OOS local WPE app started: %s\n", uri);
  g_free(uri);

  GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
  RunnerState state{loop, &device->keyInput(), surface.viewBackend()};
  g_timeout_add(8, pumpLocalEvents, &state);
  g_unix_signal_add(SIGINT, stopRunner, loop);
  g_unix_signal_add(SIGTERM, stopRunner, loop);
  g_main_loop_run(loop);
  std::fprintf(stderr, "OOS local WPE presented %llu software frames\n",
               static_cast<unsigned long long>(surface.presentedFrames()));
  g_main_loop_unref(loop);
  g_object_unref(view);
  return surface.presentedFrames() > 0 ? 0 : 1;
}
