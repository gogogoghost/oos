#include "oos/compositor/surface.h"
#include "oos/compositor/surface_transport.h"
#include "oos/web/kaios_api_bridge.h"
#include "oos/web/local_app_server.h"
#include "oos/web/wpe_app_profile.h"
#include "oos/web/wpe_key_input.h"
#include "oos/web/wpe_surface_host.h"

#include <glib-unix.h>
#include <glib.h>
#include <jsc/jsc.h>
#include <wpe/webkit.h>
#include <wpe/wpe.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <signal.h>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <vector>

extern "C" {
typedef struct _WebKitWebViewBackend WebKitWebViewBackend;
WebKitWebViewBackend *webkit_web_view_backend_new(wpe_view_backend *,
                                                  GDestroyNotify, gpointer);
}

namespace {

constexpr uint64_t kWebSurfaceId = 1;

struct RunnerOptions {
  const char *app_id = nullptr;
  const char *package_path = nullptr;
  const char *entrypoint = nullptr;
  const char *api_profile = nullptr;
  const char *data_directory = nullptr;
  const char *cache_directory = nullptr;
  int surface_fd = -1;
  int input_fd = -1;
  int api_fd = -1;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<std::string> permissions;
};

void printUsage(const char *program) {
  std::fprintf(stderr,
               "usage: %s --id ID --package APP.zip --entrypoint PATH "
               "--api-profile PROFILE --data DIR --cache DIR "
               "--surface-fd FD --input-fd FD --api-fd FD "
               "[--permission NAME ...] "
               "--width PIXELS --height PIXELS\n",
               program);
}

bool parseDimension(const char *value, uint32_t &result) {
  if (!value || !value[0])
    return false;
  char *end = nullptr;
  errno = 0;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (errno || !end || *end || parsed == 0 ||
      parsed > std::numeric_limits<uint32_t>::max())
    return false;
  result = static_cast<uint32_t>(parsed);
  return true;
}

bool environmentEnabled(const char *name) {
  const char *value = std::getenv(name);
  return value && value[0] && std::strcmp(value, "0") != 0;
}

bool parseFileDescriptor(const char *value, int &result) {
  if (!value || !value[0])
    return false;
  char *end = nullptr;
  errno = 0;
  const long parsed = std::strtol(value, &end, 10);
  if (errno || !end || *end || parsed < 0 || parsed > INT_MAX)
    return false;
  result = static_cast<int>(parsed);
  return true;
}

bool markCloseOnExec(int fd) {
  const int flags = fcntl(fd, F_GETFD);
  return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool parseOptions(int argc, char **argv, RunnerOptions &options) {
  for (int index = 1; index < argc; index += 2) {
    if (index + 1 >= argc)
      return false;
    const char *name = argv[index];
    const char *value = argv[index + 1];
    if (std::strcmp(name, "--id") == 0)
      options.app_id = value;
    else if (std::strcmp(name, "--package") == 0)
      options.package_path = value;
    else if (std::strcmp(name, "--entrypoint") == 0)
      options.entrypoint = value;
    else if (std::strcmp(name, "--api-profile") == 0)
      options.api_profile = value;
    else if (std::strcmp(name, "--permission") == 0)
      options.permissions.emplace_back(value);
    else if (std::strcmp(name, "--data") == 0)
      options.data_directory = value;
    else if (std::strcmp(name, "--cache") == 0)
      options.cache_directory = value;
    else if (std::strcmp(name, "--surface-fd") == 0) {
      if (!parseFileDescriptor(value, options.surface_fd))
        return false;
    } else if (std::strcmp(name, "--input-fd") == 0) {
      if (!parseFileDescriptor(value, options.input_fd))
        return false;
    } else if (std::strcmp(name, "--api-fd") == 0) {
      if (!parseFileDescriptor(value, options.api_fd))
        return false;
    } else if (std::strcmp(name, "--width") == 0) {
      if (!parseDimension(value, options.width))
        return false;
    } else if (std::strcmp(name, "--height") == 0) {
      if (!parseDimension(value, options.height))
        return false;
    } else {
      return false;
    }
  }
  return options.app_id && options.package_path && options.entrypoint &&
         options.api_profile && options.data_directory &&
         options.cache_directory && options.surface_fd >= 0 &&
         options.input_fd >= 0 && options.api_fd >= 0 && options.width &&
         options.height;
}

class TransportSurfaceSink final : public oos::compositor::SurfaceSink {
public:
  explicit TransportSurfaceSink(int socket_fd) : socket_fd_(socket_fd) {}
  ~TransportSurfaceSink() override {
    if (socket_fd_ >= 0)
      close(socket_fd_);
  }

  bool presentSurface(const oos::compositor::SurfaceFrame &frame) override {
    const auto [buffer_id, inserted] =
        buffer_ids_.try_emplace(frame.buffer, next_buffer_id_++);
    OosSurfaceTransportFrame transport_frame = {
        .surface_id = frame.surface_id,
        .buffer_id = buffer_id->second,
        .width = frame.width,
        .height = frame.height,
        .flags = inserted ? OOS_SURFACE_FRAME_NEW_BUFFER : 0u,
        .reserved = 0,
    };
    const int result =
        oos_surface_transport_send(socket_fd_, &transport_frame,
                                   static_cast<AHardwareBuffer *>(frame.buffer),
                                   frame.acquire_fence_fd, 5000);
    if (result != 0) {
      if (inserted)
        buffer_ids_.erase(buffer_id);
      std::fprintf(stderr, "WPE surface transport failed: %d (%s)\n", result,
                   std::strerror(-result));
    }
    return result == 0;
  }

private:
  int socket_fd_ = -1;
  uint64_t next_buffer_id_ = 1;
  std::unordered_map<void *, uint64_t> buffer_ids_;
};

struct RunnerState {
  GMainLoop *loop = nullptr;
  wpe_view_backend *backend = nullptr;
  WebKitWebView *view = nullptr;
  int input_fd = -1;
  guint input_source = 0;
  bool api_ready = false;
  bool load_failed = false;
  bool web_process_failed = false;
  bool trace_keys = false;
};

void apiCheckFinished(GObject *, GAsyncResult *result, gpointer data) {
  auto *state = static_cast<RunnerState *>(data);
  GError *error = nullptr;
  JSCValue *value =
      webkit_web_view_evaluate_javascript_finish(state->view, result, &error);
  if (!value) {
    std::fprintf(stderr, "KaiOS API self-check failed: %s\n",
                 error ? error->message : "unknown evaluation error");
    if (error)
      g_error_free(error);
    return;
  }
  char *status = jsc_value_to_string(value);
  state->api_ready = status && std::strstr(status, "\"ready\":true");
  std::fprintf(stderr, "KaiOS API self-check: %s\n",
               status ? status : "(null)");
  g_free(status);
  g_object_unref(value);
}

void loadChanged(WebKitWebView *view, WebKitLoadEvent event, gpointer data) {
  if (event != WEBKIT_LOAD_FINISHED)
    return;
  auto *state = static_cast<RunnerState *>(data);
  constexpr const char *check =
      "JSON.stringify({ready:!!globalThis.__oosRuntime&&"
      "__oosRuntime.bridgeVersion===3&&typeof WebAssembly==='object'&&"
      "typeof indexedDB==='object'&&"
      "(__oosRuntime.apiProfile!=='kaios-v3'||(!!navigator.b2g&&"
      "typeof globalThis.lib_devicecapability?.DeviceCapabilityManager?.get==="
      "'function'))&&"
      "(!__oosRuntime.permissions.some(p=>p.startsWith('device-storage:'))||"
      "(__oosRuntime.apiProfile==='kaios-v3'"
      "?typeof navigator.b2g?.getDeviceStorage==='function'"
      ":typeof navigator.getDeviceStorage==='function')),"
      "profile:globalThis.__oosRuntime&&__oosRuntime.apiProfile,"
      "b2g:!!navigator.b2g,wasm:typeof WebAssembly,"
      "indexedDB:typeof indexedDB,audioContext:typeof AudioContext})";
  webkit_web_view_evaluate_javascript(view, check, -1, nullptr, nullptr,
                                      nullptr, apiCheckFinished, state);
}

gboolean loadFailed(WebKitWebView *, WebKitLoadEvent, const char *uri,
                    GError *error, gpointer data) {
  auto *state = static_cast<RunnerState *>(data);
  state->load_failed = true;
  std::fprintf(stderr, "WPE main document load failed: uri=%s error=%s\n",
               uri ? uri : "(null)", error ? error->message : "unknown");
  return FALSE;
}

void webProcessTerminated(WebKitWebView *, WebKitWebProcessTerminationReason,
                          gpointer data) {
  auto *state = static_cast<RunnerState *>(data);
  state->web_process_failed = true;
  std::fprintf(stderr, "WPE WebProcess terminated unexpectedly\n");
  g_main_loop_quit(state->loop);
}

gboolean receiveInput(gint, GIOCondition condition, gpointer data) {
  auto *state = static_cast<RunnerState *>(data);
  if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0) {
    state->input_source = 0;
    g_main_loop_quit(state->loop);
    return G_SOURCE_REMOVE;
  }
  while (true) {
    OosSurfaceTransportKey key = {};
    const int result =
        oos_surface_transport_receive_key(state->input_fd, &key, 0);
    if (result == -ETIMEDOUT)
      return G_SOURCE_CONTINUE;
    if (result <= 0) {
      state->input_source = 0;
      g_main_loop_quit(state->loop);
      return G_SOURCE_REMOVE;
    }
    const bool dispatched = oos::web::dispatchWpeKey(state->backend, key);
    if (state->trace_keys) {
      std::fprintf(stderr,
                   "WPE key receive: code=%u action=%u symbol=0x%x "
                   "dispatched=%d\n",
                   key.code, key.action, oos::web::wpeKeySymbol(key.code),
                   dispatched ? 1 : 0);
    }
  }
}

gboolean stopRunner(gpointer data) {
  g_main_loop_quit(static_cast<GMainLoop *>(data));
  return G_SOURCE_REMOVE;
}

void closeFromWeb(void *data) {
  g_main_loop_quit(static_cast<GMainLoop *>(data));
}

void keepBackendOwnedByRunner(gpointer) {}

} // namespace

int main(int argc, char **argv) {
  RunnerOptions options;
  if (!parseOptions(argc, argv, options)) {
    printUsage(argv[0]);
    return 2;
  }
  if (!markCloseOnExec(options.surface_fd) ||
      !markCloseOnExec(options.input_fd) || !markCloseOnExec(options.api_fd)) {
    std::fprintf(stderr, "secure inherited WPE channels failed: %s\n",
                 std::strerror(errno));
    return 1;
  }

  TransportSurfaceSink surface_sink(options.surface_fd);
  oos::web::WpeSurfaceHost surface(surface_sink, kWebSurfaceId, options.width,
                                   options.height);
  if (!surface.initialize()) {
    std::fprintf(stderr, "initialize WPE surface backend failed\n");
    return 1;
  }

  GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
  oos::web::KaiOsApiBridge bridge(options.app_id, options.api_profile,
                                  std::move(options.permissions),
                                  options.api_fd);
  if (!bridge.initialize(closeFromWeb, loop)) {
    std::fprintf(stderr, "initialize KaiOS API bridge failed: %s\n",
                 bridge.lastError().c_str());
    g_main_loop_unref(loop);
    return 1;
  }
  oos::web::LocalAppServer app_server(options.app_id, options.package_path,
                                      options.entrypoint);
  if (!app_server.start()) {
    std::fprintf(stderr, "initialize KaiOS app HTTP origin failed: %s\n",
                 app_server.lastError().c_str());
    g_main_loop_unref(loop);
    return 1;
  }
  oos::web::WpeAppProfile profile(options.app_id, options.data_directory,
                                  options.cache_directory);
  if (!profile.initialize()) {
    std::fprintf(stderr, "initialize WPE app profile failed: %s\n",
                 profile.lastError().c_str());
    g_main_loop_unref(loop);
    return 1;
  }

  WebKitWebViewBackend *wrapped = webkit_web_view_backend_new(
      surface.viewBackend(), keepBackendOwnedByRunner, nullptr);
  WebKitWebView *view = profile.createView(wrapped, bridge.contentManager());
  if (!view) {
    std::fprintf(stderr, "create WPE WebView failed\n");
    g_main_loop_unref(loop);
    return 1;
  }
  WebKitSettings *settings = webkit_web_view_get_settings(view);
  if (environmentEnabled("OOS_ENABLE_INSPECTOR"))
    webkit_settings_set_enable_developer_extras(settings, TRUE);
  if (environmentEnabled("OOS_TRACE_WEB_CONSOLE")) {
    webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
  }
  RunnerState state{loop, surface.viewBackend(), view, options.input_fd};
  state.trace_keys = std::getenv("OOS_TRACE_KEYS") != nullptr;
  g_signal_connect(view, "load-changed", G_CALLBACK(loadChanged), &state);
  g_signal_connect(view, "load-failed", G_CALLBACK(loadFailed), &state);
  g_signal_connect(view, "web-process-terminated",
                   G_CALLBACK(webProcessTerminated), &state);
  state.input_source = g_unix_fd_add(
      state.input_fd, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
      receiveInput, &state);
  g_unix_signal_add(SIGINT, stopRunner, loop);
  g_unix_signal_add(SIGTERM, stopRunner, loop);
  const std::string uri = app_server.urlFor(options.entrypoint);
  webkit_web_view_load_uri(view, uri.c_str());
  std::fprintf(stderr, "OOS WPE app started: id=%s profile=%s uri=%s\n",
               options.app_id, options.api_profile, uri.c_str());
  g_main_loop_run(loop);
  if (state.input_source)
    g_source_remove(state.input_source);
  close(options.input_fd);
  g_object_unref(view);
  g_main_loop_unref(loop);
  std::fprintf(stderr, "OOS WPE app stopped: frames=%llu\n",
               static_cast<unsigned long long>(surface.presentedFrames()));
  const bool success = surface.presentedFrames() > 0 && state.api_ready &&
                       !state.load_failed && !state.web_process_failed;
  return success ? 0 : 1;
}
