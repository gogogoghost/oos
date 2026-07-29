#include "oos/apps/app_repository.h"
#include "oos/apps/permissions.h"
#include "oos/compositor/compositor.h"
#include "oos/device/service_provider.h"
#include "oos/device/device.h"
#include "oos/input/key_input.h"
#include "oos/services/system_service.h"
#include "oos/storage/app_storage.h"
#include "oos/storage/device_storage.h"
#include "oos/web/device_api_service.h"
#include "oos/web/device_api_transport.h"
#include "oos/web/kaios_api_bridge.h"
#include "oos/web/local_app_server.h"
#include "oos/web/wpe_app_profile.h"
#include "oos/web/wpe_surface_host.h"

#include <glib-unix.h>
#include <glib.h>
#include <wpe/webkit.h>
#include <wpe/wpe.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

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

void closeFromWeb(void *data) {
  g_main_loop_quit(static_cast<GMainLoop *>(data));
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
  std::unique_ptr<oos::web::LocalAppServer> app_server;
  std::string registered_uri;
  if (use_registered_app) {
    app_server = std::make_unique<oos::web::LocalAppServer>(
        app_id, registered_app.app.package_path,
        registered_app.app.manifest.entrypoint);
    if (!app_server->start()) {
      std::fprintf(stderr, "failed to serve WPE ZIP application: %s\n",
                   app_server->lastError().c_str());
      return 1;
    }
    registered_uri = app_server->urlFor(registered_app.app.manifest.entrypoint);
    path = registered_uri.c_str();
  }
  oos::web::WpeAppProfile profile(app_id, data_directory, cache_directory);
  if (!profile.initialize()) {
    std::fprintf(stderr, "failed to initialize WPE app profile: %s\n",
                 profile.lastError().c_str());
    return 1;
  }
  GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
  int api_sockets[2] = {-1, -1};
  std::unique_ptr<oos::web::KaiOsApiBridge> bridge;
  std::atomic<bool> stop_api{false};
  bool api_connected = false;
  std::string api_error;
  std::thread api_thread;
  std::shared_ptr<oos::device::ServiceProvider> services;
  std::shared_ptr<oos::storage::AppStorage> app_storage;
  std::shared_ptr<oos::services::SystemServiceHub> system_services;
  std::shared_ptr<oos::web::DeviceApiContext> device_api_context;
  if (use_registered_app) {
    const int socket_result = oos_device_api_socket_pair(api_sockets);
    if (socket_result != 0) {
      std::fprintf(stderr, "failed to create local WPE device API socket: %s\n",
                   std::strerror(-socket_result));
      g_main_loop_unref(loop);
      return 1;
    }
    bridge = std::make_unique<oos::web::KaiOsApiBridge>(
        app_id, registered_app.app.manifest.api_profile,
        registered_app.app.manifest.requested_permissions, api_sockets[1]);
    api_sockets[1] = -1;
    if (!bridge->initialize(closeFromWeb, loop)) {
      std::fprintf(stderr, "failed to initialize local KaiOS API bridge: %s\n",
                   bridge->lastError().c_str());
      close(api_sockets[0]);
      g_main_loop_unref(loop);
      return 1;
    }
    api_connected = true;
    services = std::make_shared<oos::device::ServiceProvider>(*device);
    system_services = std::make_shared<oos::services::SystemServiceHub>(
        data_root, repository.get());
    if (!system_services->initialize()) {
      std::fprintf(stderr, "failed to initialize OOS system services: %s\n",
                   system_services->lastError().c_str());
      close(api_sockets[0]);
      g_main_loop_unref(loop);
      return 1;
    }
    const std::vector<oos::apps::DataStoreGrant> data_store_grants =
        oos::apps::ownedDataStoreGrants(
            registered_app.app.manifest.requested_permissions);
    if (!data_store_grants.empty()) {
      app_storage = std::make_shared<oos::storage::AppStorage>(
          registered_app.data_directory + "/oos-platform");
      if (!app_storage->initialize()) {
        std::fprintf(stderr, "failed to initialize KaiOS DataStore: %s\n",
                     app_storage->lastError().c_str());
        close(api_sockets[0]);
        g_main_loop_unref(loop);
        return 1;
      }
    }
    device_api_context = std::make_shared<oos::web::DeviceApiContext>();
    device_api_context->services = services.get();
    device_api_context->device = device.get();
    device_api_context->app_storage = app_storage.get();
    device_api_context->system_services = system_services.get();
    device_api_context->app_id = registered_app.app.manifest.id;
    device_api_context->permissions =
        registered_app.app.manifest.requested_permissions;
    device_api_context->permission_mask =
        oos::apps::deviceServicePermissionMask(
            registered_app.app.manifest.requested_permissions);
    for (const oos::apps::DataStoreGrant &grant : data_store_grants)
      device_api_context->owned_data_stores.emplace(grant.name,
                                                    grant.writable);
    const std::string internal_media =
        std::string(data_root) + "/media/internal";
    const std::string removable_media =
        std::string(data_root) + "/media/removable";
    api_thread = std::thread([&, internal_media, removable_media, services,
                              app_storage, system_services, device_api_context] {
      oos::storage::DeviceStorageService storage(internal_media,
                                                 removable_media);
      while (!stop_api && api_connected) {
        if (!oos::web::serviceDeviceApi(api_sockets[0], storage, api_connected,
                                        api_error, 50,
                                        device_api_context.get())) {
          g_main_loop_quit(loop);
          break;
        }
      }
    });
  }
  WebKitWebView *view =
      profile.createView(wrapped, bridge ? bridge->contentManager() : nullptr);
  char *uri = appUri(path);
  if (!view || !uri) {
    if (view)
      g_object_unref(view);
    g_free(uri);
    stop_api = true;
    if (api_thread.joinable())
      api_thread.join();
    if (api_sockets[0] >= 0)
      close(api_sockets[0]);
    if (device_api_context) {
      for (const std::string &wake_lock : device_api_context->wake_locks)
        services->releaseWakeLock(wake_lock);
    }
    g_main_loop_unref(loop);
    return 1;
  }
  webkit_web_view_load_uri(view, uri);
  std::fprintf(stderr, "OOS local WPE app started: %s\n", uri);
  g_free(uri);

  RunnerState state{loop, &device->keyInput(), surface.viewBackend()};
  g_timeout_add(8, pumpLocalEvents, &state);
  g_unix_signal_add(SIGINT, stopRunner, loop);
  g_unix_signal_add(SIGTERM, stopRunner, loop);
  g_main_loop_run(loop);
  stop_api = true;
  if (api_thread.joinable())
    api_thread.join();
  if (api_sockets[0] >= 0)
    close(api_sockets[0]);
  if (use_registered_app && device_api_context) {
    for (const std::string &wake_lock : device_api_context->wake_locks)
      services->releaseWakeLock(wake_lock);
  }
  if (!api_error.empty())
    std::fprintf(stderr, "local WPE device API failed: %s\n",
                 api_error.c_str());
  std::fprintf(stderr, "OOS local WPE presented %llu software frames\n",
               static_cast<unsigned long long>(surface.presentedFrames()));
  g_main_loop_unref(loop);
  g_object_unref(view);
  return surface.presentedFrames() > 0 && api_error.empty() ? 0 : 1;
}
