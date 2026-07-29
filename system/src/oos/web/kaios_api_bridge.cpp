#include "oos/web/kaios_api_bridge.h"

#include "oos/apps/json.h"
#include "oos/web/device_api_transport.h"

#include <glib-object.h>
#include <jsc/jsc.h>
#include <wpe/webkit.h>

#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace oos::web {

namespace {

bool environmentEnabled(const char *name) {
  const char *value = std::getenv(name);
  return value && value[0] && std::strcmp(value, "0") != 0;
}

std::string jsonString(const std::string &value) {
  static const char hex[] = "0123456789abcdef";
  std::string output = "\"";
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20) {
        output += "\\u00";
        output.push_back(hex[character >> 4]);
        output.push_back(hex[character & 0x0f]);
      } else {
        output.push_back(static_cast<char>(character));
      }
    }
  }
  output.push_back('"');
  return output;
}

std::string jsonStrings(const std::vector<std::string> &values) {
  std::string output = "[";
  for (const std::string &value : values) {
    if (output.size() > 1)
      output.push_back(',');
    output += jsonString(value);
  }
  output.push_back(']');
  return output;
}

} // namespace

class DeviceApiClient {
public:
  static constexpr size_t kMaximumPendingRequests = 64;

  explicit DeviceApiClient(int socket_fd)
      : socket_fd_(socket_fd),
        trace_(environmentEnabled("OOS_TRACE_DEVICE_API")),
        worker_(&DeviceApiClient::run, this) {}

  ~DeviceApiClient() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    ready_.notify_one();
    shutdown(socket_fd_, SHUT_RDWR);
    worker_.join();
  }

  bool enqueue(uint16_t operation, uint16_t volume, uint16_t flags,
               std::string path, const void *payload, uint32_t payload_size,
               JSCValue *message, WebKitScriptMessageReply *reply) {
    Pending pending;
    pending.operation = operation;
    pending.volume = volume;
    pending.flags = flags;
    pending.path = std::move(path);
    if (payload_size) {
      const auto *bytes = static_cast<const uint8_t *>(payload);
      pending.payload.assign(bytes, bytes + payload_size);
    }
    pending.context =
        static_cast<JSCContext *>(g_object_ref(jsc_value_get_context(message)));
    pending.reply = webkit_script_message_reply_ref(reply);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_ || pending_.size() >= kMaximumPendingRequests) {
        g_object_unref(pending.context);
        webkit_script_message_reply_unref(pending.reply);
        return false;
      }
      pending_.push_back(std::move(pending));
    }
    ready_.notify_one();
    return true;
  }

private:
  struct Pending {
    uint16_t operation = 0;
    uint16_t volume = 0;
    uint16_t flags = 0;
    std::string path;
    std::vector<uint8_t> payload;
    JSCContext *context = nullptr;
    WebKitScriptMessageReply *reply = nullptr;
  };

  struct Completion {
    JSCContext *context = nullptr;
    WebKitScriptMessageReply *reply = nullptr;
    int result = 0;
    std::string response;
  };

  static gboolean finish(gpointer data) {
    std::unique_ptr<Completion> completion(static_cast<Completion *>(data));
    if (completion->result != 0) {
      webkit_script_message_reply_return_error_message(
          completion->reply, completion->result < 0
                                 ? std::strerror(-completion->result)
                                 : "OOS device API failed");
    } else {
      JSCValue *value = jsc_value_new_string(completion->context,
                                             completion->response.c_str());
      webkit_script_message_reply_return_value(completion->reply, value);
      g_object_unref(value);
    }
    webkit_script_message_reply_unref(completion->reply);
    g_object_unref(completion->context);
    return G_SOURCE_REMOVE;
  }

  void run() {
    while (true) {
      Pending request;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
        if (stopping_) {
          while (!pending_.empty()) {
            auto completion = std::make_unique<Completion>();
            completion->context = pending_.front().context;
            completion->reply = pending_.front().reply;
            completion->result = -ECANCELED;
            pending_.pop_front();
            g_main_context_invoke(nullptr, finish, completion.release());
          }
          return;
        }
        request = std::move(pending_.front());
        pending_.pop_front();
      }
      void *payload = nullptr;
      uint32_t payload_size = 0;
      auto completion = std::make_unique<Completion>();
      completion->context = request.context;
      completion->reply = request.reply;
      completion->result = oos_device_api_request_with_payload(
          socket_fd_, request.operation, request.volume, request.flags,
          request.path.c_str(), request.payload.data(),
          static_cast<uint32_t>(request.payload.size()), &payload,
          &payload_size, 30000);
      if (completion->result == 0) {
        if (request.operation == OOS_DEVICE_API_LIST_FILES ||
            request.operation == OOS_DEVICE_API_PLATFORM_CALL) {
          if (payload_size)
            completion->response.assign(static_cast<const char *>(payload),
                                        payload_size);
        } else if (request.operation == OOS_DEVICE_API_READ_FILE) {
          char *encoded = reinterpret_cast<char *>(g_base64_encode(
              static_cast<const guchar *>(payload), payload_size));
          completion->response = encoded ? encoded : "";
          g_free(encoded);
        } else if (request.operation == OOS_DEVICE_API_FREE_SPACE ||
                   request.operation == OOS_DEVICE_API_USED_SPACE) {
          if (payload_size != sizeof(uint64_t)) {
            completion->result = -EPROTO;
          } else {
            uint64_t bytes = 0;
            std::memcpy(&bytes, payload, sizeof(bytes));
            completion->response = std::to_string(bytes);
          }
        }
      }
      oos_device_api_free(payload);
      if (trace_) {
        std::fprintf(stderr,
                     "OOS device API: operation=%u volume=%u path=%s "
                     "request=%zu response=%u result=%d\n",
                     request.operation, request.volume, request.path.c_str(),
                     request.payload.size(), payload_size, completion->result);
      }
      g_main_context_invoke(nullptr, finish, completion.release());
    }
  }

  int socket_fd_ = -1;
  bool trace_ = false;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Pending> pending_;
  bool stopping_ = false;
  std::thread worker_;
};

KaiOsApiBridge::KaiOsApiBridge(std::string app_id, std::string api_profile,
                               std::vector<std::string> permissions, int api_fd)
    : app_id_(std::move(app_id)), api_profile_(std::move(api_profile)),
      permissions_(std::move(permissions)), api_fd_(api_fd) {}

KaiOsApiBridge::~KaiOsApiBridge() {
  if (manager_) {
    webkit_user_content_manager_unregister_script_message_handler(
        manager_, "oosLifecycle", nullptr);
    webkit_user_content_manager_unregister_script_message_handler(
        manager_, "oosDeviceApi", nullptr);
    g_object_unref(manager_);
  }
  api_client_.reset();
  while (g_main_context_pending(nullptr))
    g_main_context_iteration(nullptr, FALSE);
  if (api_fd_ >= 0)
    close(api_fd_);
}

bool KaiOsApiBridge::initialize(CloseCallback close_callback,
                                void *close_context) {
  error_.clear();
  if (app_id_.empty() || api_fd_ < 0 ||
      (api_profile_ != "kaios-b2g48" && api_profile_ != "kaios-v3")) {
    error_ = "unsupported or empty KaiOS API profile";
    return false;
  }
  manager_ = webkit_user_content_manager_new();
  if (!manager_) {
    error_ = "cannot create WebKit user content manager";
    return false;
  }
  close_callback_ = close_callback;
  close_context_ = close_context;
  g_signal_connect(manager_, "script-message-received::oosLifecycle",
                   G_CALLBACK(handleLifecycle), this);
  if (!webkit_user_content_manager_register_script_message_handler(
          manager_, "oosLifecycle", nullptr)) {
    error_ = "cannot register KaiOS lifecycle bridge";
    return false;
  }
  g_signal_connect(manager_, "script-message-with-reply-received::oosDeviceApi",
                   G_CALLBACK(handleDeviceApi), this);
  if (!webkit_user_content_manager_register_script_message_handler_with_reply(
          manager_, "oosDeviceApi", nullptr)) {
    error_ = "cannot register OOS device API bridge";
    return false;
  }
  api_client_ = std::make_unique<DeviceApiClient>(api_fd_);

  script_ = "(() => { 'use strict';"
            "const runtime = Object.freeze({appId:" +
            jsonString(app_id_) + ",apiProfile:" + jsonString(api_profile_) +
            ",permissions:Object.freeze(" + jsonStrings(permissions_) +
            "),bridgeVersion:3,traceKeys:" +
            (std::getenv("OOS_TRACE_KEYS") ? "true" : "false") +
            "});"
            "Object.defineProperty(globalThis,'__oosRuntime',"
            "{value:runtime,writable:false,configurable:false});"
            "Object.defineProperty(window,'close',{value:()=>"
            "window.webkit.messageHandlers.oosLifecycle.postMessage('close'),"
            "writable:false,configurable:false});"
            R"JS(
const nav = globalThis.navigator;
const defer = typeof queueMicrotask === 'function'
  ? queueMicrotask : callback => Promise.resolve().then(callback);
// libwpe has no SoftLeft/SoftRight keysyms. Its closest legacy key values are
// produced by the KaiOS keypad adapter, so normalize them before app handlers
// observe the event.
const kaiKeyNames = new Map([
  ['ContextMenu', 'SoftLeft'],
  ['Backspace', 'SoftRight']
]);
const translatedKeys = new WeakSet();
if (runtime.traceKeys) {
  const trace = message =>
    window.webkit.messageHandlers.oosLifecycle.postMessage(`trace:${message}`);
  const traceDom = reason => defer(() => {
    const active = document.activeElement?.tagName ?? 'none';
    const body = (document.body?.innerText ?? '').replace(/\s+/g, ' ')
      .trim().slice(0, 240);
    trace(`${reason}:active=${active}:body=${body}`);
  });
  for (const type of ['keydown', 'keyup']) {
    globalThis.addEventListener(type, event => {
      trace(`${type}:key=${event.key}:code=${event.code}:repeat=${event.repeat}`);
      if (type === 'keyup') traceDom('post-key');
    }, true);
  }
  globalThis.addEventListener('focus', () => trace('window-focus'), true);
  globalThis.addEventListener('blur', () => trace('window-blur'), true);
  document.addEventListener('visibilitychange', () =>
    trace(`visibility=${document.visibilityState}`), true);
}
for (const type of ['keydown', 'keyup']) {
  globalThis.addEventListener(type, event => {
    if (translatedKeys.has(event) || !kaiKeyNames.has(event.key)) return;
    event.preventDefault();
    event.stopImmediatePropagation();
    const translated = new KeyboardEvent(type, {
      key: kaiKeyNames.get(event.key), code: kaiKeyNames.get(event.key),
      location: event.location, repeat: event.repeat,
      ctrlKey: event.ctrlKey, shiftKey: event.shiftKey,
      altKey: event.altKey, metaKey: event.metaKey,
      bubbles: true, cancelable: true, composed: true
    });
    translatedKeys.add(translated);
    event.target.dispatchEvent(translated);
  }, true);
}
const define = (target, name, value) => {
  if (target && !(name in target))
    Object.defineProperty(target, name, { value, writable: true,
      configurable: true, enumerable: true });
};
const notSupported = name => {
  const message = `${name} is not implemented by OOS`;
  if (typeof DOMException === 'function')
    return new DOMException(message, 'NotSupportedError');
  const error = new Error(message);
  error.name = 'NotSupportedError';
  return error;
};
const promiseRequest = promise => {
  const request = { result: null, error: null, onsuccess: null, onerror: null,
    readyState: 'pending' };
  Promise.resolve(promise).then(result => {
    request.readyState = 'done';
    request.result = result;
    if (typeof request.onsuccess === 'function')
      request.onsuccess.call(request, { type: 'success', target: request });
  }, error => {
    request.readyState = 'done';
    request.error = error;
    if (typeof request.onerror === 'function')
      request.onerror.call(request, { type: 'error', target: request });
  });
  return request;
};
const domRequest = (result, error = null) => promiseRequest(error
  ? Promise.reject(error) : Promise.resolve(result));
const emptyCursor = () => {
  const cursor = { result: null, error: null, done: true, onsuccess: null,
    onerror: null, continue() {} };
  defer(() => {
    if (typeof cursor.onsuccess === 'function')
      cursor.onsuccess.call(cursor, { type: 'success', target: cursor });
  });
  return cursor;
};
const eventTarget = target => {
  const listeners = new Map();
  target.addEventListener = (type, callback) => {
    if (typeof callback !== 'function') return;
    const callbacks = listeners.get(type) || [];
    callbacks.push(callback);
    listeners.set(type, callbacks);
  };
  target.removeEventListener = (type, callback) => {
    const callbacks = listeners.get(type) || [];
    listeners.set(type, callbacks.filter(entry => entry !== callback));
  };
  target.dispatchEvent = event => {
    for (const callback of listeners.get(event?.type) || [])
      callback.call(target, event);
    const handler = target[`on${event?.type}`];
    if (typeof handler === 'function') handler.call(target, event);
    return true;
  };
  return target;
};
const failedRequest = name => domRequest(null, notSupported(name));
const failedPromise = name => Promise.reject(notSupported(name));
const deviceHandler = globalThis.webkit?.messageHandlers?.oosDeviceApi;
const deviceCall = (operation, volume, path = '', data = '', mode = '') => {
  if (!deviceHandler)
    return Promise.reject(notSupported('OOS device API bridge'));
  return deviceHandler.postMessage(JSON.stringify({ operation, volume, path,
    data, mode }));
};
const decodeBase64 = encoded => {
  const binary = globalThis.atob(encoded);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; ++index)
    bytes[index] = binary.charCodeAt(index);
  return bytes;
};
const encodeBase64 = bytes => {
  let binary = '';
  for (let index = 0; index < bytes.length; index += 0x4000)
    binary += String.fromCharCode(...bytes.subarray(index, index + 0x4000));
  return btoa(binary);
};
const deviceFileMetadata = Symbol('OOS DeviceStorage file');
const makeDeviceFile = (volume, entry, bytes = null) => {
  const contents = bytes ? [bytes] : [];
  const file = new File(contents, entry.path, {
    type: 'application/octet-stream', lastModified: entry.lastModified || 0
  });
  if (!bytes) {
    try { Object.defineProperty(file, 'size', { value: entry.size }); }
    catch (_) {}
  }
  Object.defineProperty(file, deviceFileMetadata, {
    value: { volume, path: entry.path }, enumerable: false
  });
  return file;
};
const listDeviceFiles = async volume => {
  const encoded = await deviceCall('list', volume);
  const entries = JSON.parse(encoded);
  if (!Array.isArray(entries)) throw new Error('Invalid device storage list');
  return entries.map(entry => makeDeviceFile(volume, entry));
};
const readDeviceFile = async metadata =>
  decodeBase64(await deviceCall('read', metadata.volume, metadata.path));
const writeDeviceFile = async (volume, blob, path, mode) => {
  if (!(blob instanceof Blob)) throw new TypeError('DeviceStorage expects a Blob');
  const bytes = new Uint8Array(await blob.arrayBuffer());
  if (bytes.length > 64 * 1024 * 1024)
    throw new DOMException('DeviceStorage file exceeds 64 MiB', 'QuotaExceededError');
  await deviceCall('write', volume, path, encodeBase64(bytes), mode);
  return path;
};
const makeDeviceCursor = (volume, prefix = '') => {
  const files = listDeviceFiles(volume).then(entries => prefix
    ? entries.filter(file => file[deviceFileMetadata].path.startsWith(prefix))
    : entries);
  let cursorIndex = 0;
  let pending = false;
  const cursor = { result: null, error: null, done: false, onsuccess: null,
    onerror: null,
    continue() {
      if (!pending && !cursor.done) {
        pending = true;
        defer(emitNext);
      }
    },
    values() {
      let index = 0;
      return {
        async next() {
          const entries = await files;
          return index < entries.length
            ? { value: entries[index++], done: false }
            : { value: undefined, done: true };
        },
        [Symbol.asyncIterator]() { return this; }
      };
    },
    [Symbol.asyncIterator]() { return this.values(); }
  };
  const emitNext = async () => {
    pending = false;
    try {
      const entries = await files;
      cursor.result = cursorIndex < entries.length
        ? entries[cursorIndex++] : null;
      cursor.done = cursor.result === null;
      if (typeof cursor.onsuccess === 'function')
        cursor.onsuccess.call(cursor, { type: 'success', target: cursor });
    } catch (error) {
      cursor.error = error;
      cursor.done = true;
      if (typeof cursor.onerror === 'function')
        cursor.onerror.call(cursor, { type: 'error', target: cursor });
    }
  };
  pending = true;
  defer(emitNext);
  return cursor;
};
const NativeFileReader = globalThis.FileReader;
class OosFileReader {
  constructor() {
    this.readyState = 0; this.result = null; this.error = null;
    this.onloadstart = null; this.onprogress = null; this.onload = null;
    this.onerror = null; this.onabort = null; this.onloadend = null;
    this._events = eventTarget(this);
    this._native = null;
  }
  _emit(type) {
    this.dispatchEvent({ type, target: this, loaded: 0, total: 0 });
  }
  _read(file, mode, encoding) {
    const metadata = file?.[deviceFileMetadata];
    if (!metadata) {
      const native = this._native = new NativeFileReader();
      for (const type of ['loadstart', 'progress', 'load', 'error', 'abort', 'loadend'])
        native[`on${type}`] = () => {
          this.readyState = native.readyState; this.result = native.result;
          this.error = native.error; this._emit(type);
        };
      native[mode](file, encoding);
      return;
    }
    this.readyState = 1;
    this._emit('loadstart');
    readDeviceFile(metadata).then(bytes => {
      if (mode === 'readAsText')
        this.result = new TextDecoder(encoding || 'utf-8').decode(bytes);
      else if (mode === 'readAsDataURL') {
        let binary = '';
        for (let index = 0; index < bytes.length; index += 0x4000)
          binary += String.fromCharCode(...bytes.subarray(index, index + 0x4000));
        this.result = `data:application/octet-stream;base64,${btoa(binary)}`;
      } else
        this.result = bytes.buffer;
      this.readyState = 2;
      this._emit('load'); this._emit('loadend');
    }, error => {
      this.error = error; this.readyState = 2;
      this._emit('error'); this._emit('loadend');
    });
  }
  readAsArrayBuffer(file) { this._read(file, 'readAsArrayBuffer'); }
  readAsText(file, encoding) { this._read(file, 'readAsText', encoding); }
  readAsDataURL(file) { this._read(file, 'readAsDataURL'); }
  abort() {
    if (this._native) this._native.abort();
    this.readyState = 2; this._emit('abort'); this._emit('loadend');
  }
}
OosFileReader.EMPTY = OosFileReader.prototype.EMPTY = 0;
OosFileReader.LOADING = OosFileReader.prototype.LOADING = 1;
OosFileReader.DONE = OosFileReader.prototype.DONE = 2;
try { Object.defineProperty(globalThis, 'FileReader', {
  value: OosFileReader, writable: true, configurable: true
}); } catch (_) {}
let generatedDeviceFile = 0;
const storage = (storageName, volume, isRemovable, isDefault) => {
  let target;
  const notify = (reason, path) => {
    target.dispatchEvent({ type: 'change', target, reason,
      path: `${storageName}/${path}` });
  };
  const store = (blob, path, mode, reason) => promiseRequest(
    writeDeviceFile(volume, blob, path, mode).then(result => {
      notify(reason, path); return result;
    }));
  target = eventTarget({
    storageName, storagePath: storageName, isRemovable, default: isDefault,
    canBeMounted: false, canBeFormatted: false, lowDiskSpace: false,
    onchange: null,
    add: blob => {
      const name = blob instanceof File && blob.name
        ? blob.name : `oos-${Date.now()}-${++generatedDeviceFile}`;
      return store(blob, name, 'create', 'created');
    },
    addNamed: (blob, path) => store(blob, path, 'create', 'created'),
    appendNamed: (blob, path) => store(blob, path, 'append', 'modified'),
    delete: path => promiseRequest(deviceCall('delete', volume, path).then(() => {
      notify('deleted', path); return path;
    })),
    get: path => promiseRequest(readDeviceFile({ volume, path }).then(bytes =>
      makeDeviceFile(volume, { path, size: bytes.length,
        lastModified: Date.now() }, bytes))),
    getEditable: () => failedRequest('DeviceStorage.getEditable'),
    getRoot: () => failedRequest('DeviceStorage.getRoot'),
    enumerate: prefix => makeDeviceCursor(volume, prefix || ''),
    enumerateEditable: prefix => makeDeviceCursor(volume, prefix || ''),
    available: () => domRequest('available'),
    storageStatus: () => domRequest('available'),
    freeSpace: () => promiseRequest(deviceCall('free-space', volume).then(Number)),
    usedSpace: () => promiseRequest(deviceCall('used-space', volume).then(Number)),
    format: () => failedRequest('DeviceStorage.format'),
    mount: () => failedRequest('DeviceStorage.mount'),
    unmount: () => failedRequest('DeviceStorage.unmount')
  });
  return target;
};
const internalStorage = storage('sdcard', 0, false, true);
const removableStorage = storage('sdcard1', 1, true, false);
const getDeviceStorages = name => name === 'sdcard'
  ? [internalStorage, removableStorage] : [storage(name, 0, false, true)];
const getDeviceStorage = name => getDeviceStorages(name)[0];
const hasPermission = (...names) => runtime.permissions.some(permission =>
  names.includes(permission));
const hasPermissionPrefix = prefix => runtime.permissions.some(permission =>
  permission.startsWith(prefix));
const platformCall = (method, parameters = {}) =>
  deviceCall('platform', 0, method, JSON.stringify(parameters)).then(encoded =>
    JSON.parse(encoded));
const systemCall = (service, operation, payload = {}) =>
  platformCall('system.request', { service, operation,
    payload: JSON.stringify(payload) });
const makeWakeLock = topic => {
  let released = false;
  platformCall('power.acquire-wake-lock', { name: String(topic) })
    .catch(() => { released = true; });
  const release = () => {
    if (released) return;
    released = true;
    platformCall('power.release-wake-lock', { name: String(topic) })
      .catch(() => {});
  };
  return { topic, get released() { return released; }, unlock: release,
    release };
};
const powerAllowed = hasPermission('power');
const storageAllowed = hasPermissionPrefix('device-storage:');
const wifiAllowed = hasPermission('wifi-manage', 'wifi');
const bluetoothAllowed = hasPermission('bluetooth');
const cameraAllowed = hasPermission('camera');
const modemAllowed = hasPermission('mobileconnection', 'mobilenetwork');
const powerSupplyAllowed = hasPermission('powersupply');
const alarmsAllowed = hasPermission('alarms');
const settingsAllowed = hasPermission('settings', 'settings:read',
  'settings:write');
const contactsAllowed = hasPermission('contacts', 'contacts:read',
  'contacts:write');
const notificationsAllowed = hasPermission('desktop-notification',
  'notification');
const applicationsAllowed = hasPermission('apps-management');
const audioPolicyAllowed = hasPermission('volume-control') ||
  hasPermissionPrefix('audio-channel-');
const inputMethodAllowed = hasPermission('input', 'input-manage');
const timeAllowed = hasPermission('system-time', 'system-time:read',
  'system-time:write');
const nfcAllowed = hasPermission('nfc', 'nfc-manager');
const secureElementAllowed = hasPermission('secureelement-manage',
  'secure-element');
const networkStatsAllowed = hasPermission('networkstats-manage');
const fmRadioAllowed = hasPermission('fmradio');
const tcpSocketAllowed = hasPermission('tcp-socket');
const telephonyAllowed = hasPermission('telephony');
const externalApiAllowed = hasPermission('external-api');
const ownedDataStoreDefinitions = new Map();
for (const permission of runtime.permissions) {
  for (const [prefix, writable] of [
    // The owner's access is always read/write. The declaration controls what
    // future cross-application consumers may do.
    ['datastore-owned:readonly:', true],
    ['datastore-owned:readwrite:', true]
  ]) {
    if (permission.startsWith(prefix) && permission.length > prefix.length)
      ownedDataStoreDefinitions.set(permission.slice(prefix.length), writable);
  }
}
const dataStoreError = (message, name = 'InvalidStateError') => {
  if (typeof DOMException === 'function') return new DOMException(message, name);
  const error = new Error(message); error.name = name; return error;
};
const cloneDataStoreValue = value => {
  if (value === undefined) throw new TypeError('DataStore cannot store undefined');
  return JSON.parse(JSON.stringify(value));
};
const dataStoreKey = id => {
  if (typeof id === 'number' && Number.isInteger(id) && id >= 0)
    return `n:${id}`;
  if (typeof id === 'string') return `s:${id}`;
  throw new TypeError('DataStore id must be an unsigned integer or string');
};
const emptyDataStoreState = () => ({ revision: 0, nextId: 1,
  records: {}, changes: [] });
const loadDataStoreState = async name => {
  const stored = await platformCall('datastore.get', { name });
  if (!stored.found) return emptyDataStoreState();
  const state = JSON.parse(stored.value);
  if (!state || !Number.isSafeInteger(state.revision) ||
      !Number.isSafeInteger(state.nextId) || !state.records ||
      !Array.isArray(state.changes))
    throw dataStoreError('DataStore state is corrupt', 'DataError');
  return state;
};
class OosDataStore {
  constructor(name, writable) {
    this.name = name;
    this.owner = `${globalThis.location.origin}/manifest.webapp`;
    this.readOnly = !writable;
    this.onchange = null;
    this._state = loadDataStoreState(name);
    this._serial = Promise.resolve();
    this._revision = 0;
    eventTarget(this);
    this._state.then(state => { this._revision = state.revision; });
  }
  get revisionId() { return `${runtime.appId}:${this._revision}`; }
  _checkRevision(revision) {
    if (revision !== undefined && revision !== null &&
        String(revision) !== this.revisionId)
      throw dataStoreError('DataStore revision has changed', 'ConstraintError');
  }
  _mutate(operation) {
    if (this.readOnly)
      return Promise.reject(dataStoreError('DataStore is read-only',
        'ReadOnlyError'));
    const result = this._serial.then(async () => {
      const state = await this._state;
      const changed = operation(state);
      state.revision += 1;
      this._revision = state.revision;
      const revisionId = this.revisionId;
      const change = { revision: state.revision, revisionId,
        operation: changed.operation, id: changed.id ?? null,
        data: changed.data };
      state.changes.push(change);
      if (state.changes.length > 1024)
        state.changes.splice(0, state.changes.length - 1024);
      await platformCall('datastore.set', { name: this.name,
        value: JSON.stringify(state) });
      this.dispatchEvent({ type: 'change', target: this, revisionId,
        operation: change.operation, id: change.id,
        owner: this.owner });
      return changed.result;
    });
    this._serial = result.then(() => undefined, () => undefined);
    return result;
  }
  async get(...ids) {
    await this._serial;
    const state = await this._state;
    const values = ids.map(id => state.records[dataStoreKey(id)]?.value);
    return ids.length === 1 ? values[0] : values;
  }
  add(value, id, revisionId) {
    return this._mutate(state => {
      this._checkRevision(revisionId);
      if (id === undefined) {
        while (state.records[`n:${state.nextId}`]) state.nextId += 1;
        id = state.nextId++;
      }
      const key = dataStoreKey(id);
      if (state.records[key])
        throw dataStoreError('DataStore id already exists', 'ConstraintError');
      const data = cloneDataStoreValue(value);
      state.records[key] = { id, value: data };
      return { operation: 'add', id, data, result: id };
    });
  }
  put(value, id, revisionId) {
    return this._mutate(state => {
      this._checkRevision(revisionId);
      const key = dataStoreKey(id);
      const data = cloneDataStoreValue(value);
      state.records[key] = { id, value: data };
      return { operation: 'put', id, data, result: id };
    });
  }
  remove(...ids) {
    if (!ids.length)
      return Promise.reject(new TypeError('DataStore.remove requires an id'));
    let revisionId;
    const candidate = ids.length > 1 ? ids[ids.length - 1] : undefined;
    const revisionPrefix = `${runtime.appId}:`;
    if (typeof candidate === 'string' && candidate.startsWith(revisionPrefix) &&
        /^\d+$/.test(candidate.slice(revisionPrefix.length)))
      revisionId = ids.pop();
    return this._mutate(state => {
      this._checkRevision(revisionId);
      for (const id of ids) delete state.records[dataStoreKey(id)];
      return { operation: 'remove', id: ids.length === 1 ? ids[0] : null,
        result: true };
    });
  }
  clear(revisionId) {
    return this._mutate(state => {
      this._checkRevision(revisionId);
      state.records = {};
      return { operation: 'clear', id: null, result: undefined };
    });
  }
  async getLength() {
    await this._serial;
    return Object.keys((await this._state).records).length;
  }
  sync(revisionId) {
    let index = 0;
    const tasks = this._serial.then(async () => {
      const state = await this._state;
      if (revisionId === undefined || revisionId === null)
        return Object.values(state.records).map(record => ({
          operation: 'add', id: record.id,
          data: cloneDataStoreValue(record.value) }));
      const prefix = `${runtime.appId}:`;
      const revision = String(revisionId).startsWith(prefix)
        ? Number(String(revisionId).slice(prefix.length)) : -1;
      return state.changes.filter(change => change.revision > revision);
    });
    return { next: async () => {
      const changes = await tasks;
      return index < changes.length ? changes[index++] : { operation: 'done' };
    } };
  }
}
const ownedDataStores = new Map();
const getDataStores = async name => {
  name = String(name);
  const writable = ownedDataStoreDefinitions.get(name);
  if (writable === undefined) return [];
  if (!ownedDataStores.has(name))
    ownedDataStores.set(name, new OosDataStore(String(name), writable));
  return [ownedDataStores.get(name)];
};

let vibrationSequence = 0;
define(nav, 'vibrate', pattern => {
  const token = ++vibrationSequence;
  const values = (Array.isArray(pattern) ? pattern : [pattern])
    .slice(0, 128).map(value => Math.min(60000,
      Math.max(0, Number(value) || 0)));
  platformCall('vibrator.stop').catch(() => {});
  let offset = 0;
  for (let index = 0; index < values.length && offset < 60000; ++index) {
    const durationMs = Math.min(values[index], 60000 - offset);
    if (index % 2 === 0 && durationMs > 0)
      setTimeout(() => {
        if (token === vibrationSequence)
          platformCall('vibrator.vibrate', { durationMs }).catch(() => {});
      }, offset);
    offset += durationMs;
  }
  return true;
});

let batterySnapshot = null;
const batteryManager = eventTarget({
  charging: false, level: 0, chargingTime: Infinity,
  dischargingTime: Infinity, powerSupplyOnline: false,
  onchargingchange: null, onlevelchange: null,
  onpowersupplystatuschanged: null,
  async refresh() {
    const previousCharging = this.charging;
    const previousLevel = this.level;
    const previousPowerSupply = this.powerSupplyOnline;
    batterySnapshot = await platformCall('power.battery');
    this.charging = batterySnapshot.state === 'charging' ||
      batterySnapshot.state === 'full';
    this.powerSupplyOnline = !!batterySnapshot.usbOnline;
    this.level = Math.max(0, Math.min(1,
      batterySnapshot.capacityPercent / 100));
    if (this.charging !== previousCharging)
      this.dispatchEvent({ type: 'chargingchange', target: this });
    if (this.level !== previousLevel)
      this.dispatchEvent({ type: 'levelchange', target: this });
    if (this.powerSupplyOnline !== previousPowerSupply)
      this.dispatchEvent({ type: 'powersupplystatuschanged', target: this });
    return this;
  }
});
define(nav, 'getBattery', () => batteryManager.refresh());
batteryManager.refresh().catch(() => {});

const wifiManager = eventTarget({
  enabled: false, connection: { status: 'disconnected', network: null },
  connectionInformation: { ipAddress: '', signalStrength: 0,
    relSignalStrength: 0, linkSpeed: 0 },
  onstatuschange: null, onconnectioninfoupdate: null,
  getNetworks: () => failedRequest('WiFiManager.getNetworks'),
  getKnownNetworks: () => failedRequest('WiFiManager.getKnownNetworks'),
  associate: () => failedRequest('WiFiManager.associate'),
  forget: () => failedRequest('WiFiManager.forget'),
  disconnect: () => failedRequest('WiFiManager.disconnect'),
  reconnect: () => failedRequest('WiFiManager.reconnect'),
  wps: () => failedRequest('WiFiManager.wps'),
  setStaticIpMode: () => failedRequest('WiFiManager.setStaticIpMode'),
  setPowerSavingMode: () => failedRequest('WiFiManager.setPowerSavingMode'),
  importCert: () => failedRequest('WiFiManager.importCert'),
  deleteCert: () => failedRequest('WiFiManager.deleteCert'),
  getImportedCerts: () => failedRequest('WiFiManager.getImportedCerts'),
  setHttpProxy: () => failedRequest('WiFiManager.setHttpProxy'),
  getHttpProxy: () => failedRequest('WiFiManager.getHttpProxy'),
  refresh: () => failedPromise('WiFiManager.refresh'),
  getIpConfiguration: () => failedPromise('WiFiManager.getIpConfiguration'),
  useDhcp: () => failedPromise('WiFiManager.useDhcp')
});
const bluetoothAdapter = eventTarget({
  state: 'disabled', discovering: false, pairedDevices: [],
  ondevicefound: null,
  enable: () => failedRequest('BluetoothAdapter.enable'),
  disable: () => failedRequest('BluetoothAdapter.disable'),
  setName: () => failedRequest('BluetoothAdapter.setName'),
  setDiscoverable: () => failedRequest('BluetoothAdapter.setDiscoverable'),
  startDiscovery: () => failedRequest('BluetoothAdapter.startDiscovery'),
  stopDiscovery: () => failedRequest('BluetoothAdapter.stopDiscovery'),
  pair: () => failedRequest('BluetoothAdapter.pair'),
  unpair: () => failedRequest('BluetoothAdapter.unpair'),
  cancelPairing: () => failedRequest('BluetoothAdapter.cancelPairing'),
  fetchUuids: () => failedRequest('BluetoothAdapter.fetchUuids'),
  connect: () => failedRequest('BluetoothAdapter.connect'),
  disconnect: () => failedRequest('BluetoothAdapter.disconnect'),
  getConnectedDevices: () =>
    failedRequest('BluetoothAdapter.getConnectedDevices'),
  sendFile: () => failedRequest('BluetoothAdapter.sendFile'),
  stopSendingFile: () => failedRequest('BluetoothAdapter.stopSendingFile'),
  confirmReceivingFile: () =>
    failedRequest('BluetoothAdapter.confirmReceivingFile'),
  connectSco: () => failedRequest('BluetoothAdapter.connectSco'),
  disconnectSco: () => failedRequest('BluetoothAdapter.disconnectSco'),
  isScoConnected: () => failedRequest('BluetoothAdapter.isScoConnected'),
  leScan: () => failedPromise('BluetoothAdapter.leScan'),
  startLeScan: () => failedPromise('BluetoothAdapter.startLeScan'),
  stopLeScan: () => failedPromise('BluetoothAdapter.stopLeScan')
});
const bluetoothManager = eventTarget({ enabled: false,
  defaultAdapter: bluetoothAdapter, onenabled: null, ondisabled: null,
  enable: () => failedRequest('BluetoothManager.enable'),
  disable: () => failedRequest('BluetoothManager.disable'),
  getDefaultAdapter: () => failedPromise('BluetoothManager.getDefaultAdapter'),
  getAdapters: () => failedPromise('BluetoothManager.getAdapters') });
Object.defineProperty(wifiManager, 'enabled', { configurable: false,
  enumerable: true, get: () => false,
  set: () => { throw notSupported('WiFiManager.enabled'); } });
Object.defineProperty(bluetoothManager, 'enabled', { configurable: false,
  enumerable: true, get: () => false,
  set: () => { throw notSupported('BluetoothManager.enabled'); } });
Object.defineProperty(bluetoothManager, 'defaultAdapter', { configurable: false,
  enumerable: true, get: () => bluetoothAdapter,
  set: () => { throw notSupported('BluetoothManager.defaultAdapter'); } });

let cameraCache = [];
const camerasManager = {
  getListOfCameras: () => cameraCache.map(camera => camera.id),
  getCameraInfo: id => cameraCache.find(camera => camera.id === id) || null,
  refresh: () => platformCall('camera.enumerate').then(cameras => {
    cameraCache = cameras; return cameras;
  }),
  setTorch: (cameraId, enabled) => domRequest(platformCall('camera.set-torch',
    { cameraId: String(cameraId), enabled: !!enabled }))
};
const mobileConnection = eventTarget({
  radioState: 'unknown', cardState: 'unknown', voice: null, data: null,
  iccId: null, lastKnownNetwork: null, lastKnownHomeNetwork: null,
  networkSelectionMode: 'automatic', onvoicechange: null,
  ondatachange: null, onradiostatechange: null,
  refresh: () => failedPromise('MobileConnection.refresh'),
  setRadioEnabled: () => failedRequest('MobileConnection.setRadioEnabled'),
  getNetworks: () => failedRequest('MobileConnection.getNetworks'),
  selectNetwork: () => failedRequest('MobileConnection.selectNetwork'),
  selectNetworkAutomatically: () =>
    failedRequest('MobileConnection.selectNetworkAutomatically'),
  setPreferredNetworkType: () =>
    failedRequest('MobileConnection.setPreferredNetworkType'),
  getPreferredNetworkType: () =>
    failedRequest('MobileConnection.getPreferredNetworkType'),
  setRoamingPreference: () =>
    failedRequest('MobileConnection.setRoamingPreference'),
  getRoamingPreference: () =>
    failedRequest('MobileConnection.getRoamingPreference'),
  setVoicePrivacyMode: () =>
    failedRequest('MobileConnection.setVoicePrivacyMode'),
  getVoicePrivacyMode: () =>
    failedRequest('MobileConnection.getVoicePrivacyMode'),
  sendMMI: () => failedRequest('MobileConnection.sendMMI'),
  cancelMMI: () => failedRequest('MobileConnection.cancelMMI'),
  setCallForwardingOption: () =>
    failedRequest('MobileConnection.setCallForwardingOption'),
  getCallForwardingOption: () =>
    failedRequest('MobileConnection.getCallForwardingOption'),
  setCallBarringOption: () =>
    failedRequest('MobileConnection.setCallBarringOption'),
  getCallBarringOption: () =>
    failedRequest('MobileConnection.getCallBarringOption'),
  changeCallBarringPassword: () =>
    failedRequest('MobileConnection.changeCallBarringPassword'),
  setCallWaitingOption: () =>
    failedRequest('MobileConnection.setCallWaitingOption'),
  getCallWaitingOption: () =>
    failedRequest('MobileConnection.getCallWaitingOption'),
  getNeighboringCellIds: () =>
    failedRequest('MobileConnection.getNeighboringCellIds'),
  getCellInfoList: () => failedRequest('MobileConnection.getCellInfoList')
});
let deviceCapabilities = null;
const getCapabilities = () => deviceCapabilities
  ? Promise.resolve(deviceCapabilities)
  : platformCall('device.capabilities').then(capabilities =>
      deviceCapabilities = capabilities);
const getFeature = feature => getCapabilities().then(capabilities =>
  capabilities[String(feature)]);
const hasFeature = feature => getFeature(feature).then(state =>
  state === 'implemented' || state === 'validated');

class OosDaemonSession {
  constructor() { this.connected = false; this.state = null; }
  open(_transport, _host, _token, state) {
    this.connected = true;
    this.state = state || {};
    defer(() => this.state.onsessionconnected?.());
  }
  close() {
    if (!this.connected) return;
    this.connected = false;
    defer(() => this.state?.onsessiondisconnected?.());
  }
}
const deviceCapabilityService = Object.freeze({
  service_id: 'devicecapability',
  get: feature => getFeature(feature)
});

const encodedValue = value => JSON.stringify(value === undefined ? null : value);
const settingsObservers = new Map();
const settingsSubscriptions = new Set();
const notifySetting = (name, value) => {
  const info = { name, value };
  for (const observer of settingsObservers.get(name) || []) {
    try {
      if (typeof observer === 'function') observer(info);
      else if (typeof observer?.callback === 'function') observer.callback(info);
    } catch (_) {}
  }
  settingsService.dispatchEvent({ type: settingsService.CHANGE_EVENT,
    target: settingsService, detail: info, value: info });
  mozSettings.dispatchEvent({ type: 'settingchange', target: mozSettings,
    settingName: name, settingValue: value });
};
const readSetting = name => systemCall('settings', 'get', {
  name: String(name)
}).then(result => result.found ? result.value : undefined);
const writeSetting = (name, value) => systemCall('settings', 'set', {
  name: String(name), value: encodedValue(value)
});
const writeSettings = async settings => {
  const entries = Array.isArray(settings)
    ? settings.map(setting => [setting.name, setting.value])
    : Object.entries(settings || {});
  for (const [name, value] of entries) await writeSetting(name, value);
};
const settingsService = eventTarget({
  service_id: 'settings', CHANGE_EVENT: 'change',
  addObserver(name, observer) {
    name = String(name);
    const observers = settingsObservers.get(name) || [];
    if (!observers.includes(observer)) observers.push(observer);
    settingsObservers.set(name, observers);
    if (settingsSubscriptions.has(name)) return Promise.resolve();
    settingsSubscriptions.add(name);
    return addInternalSystemMessageHandler(`setting:${name}`, payload => {
      if (payload?.name === name) notifySetting(name, payload.value);
    });
  },
  removeObserver(name, observer) {
    name = String(name);
    settingsObservers.set(name, (settingsObservers.get(name) || [])
      .filter(candidate => candidate !== observer));
    return Promise.resolve();
  },
  clear: () => systemCall('settings', 'clear'),
  get: name => readSetting(name).then(value => {
    if (value === undefined)
      return Promise.reject({ name: String(name),
        reason: 'NON_EXISTING_SETTING' });
    return { name: String(name), value };
  }),
  getBatch: names => systemCall('settings', 'get-batch', {
    names: Array.from(names || [], String)
  }).then(values => Object.entries(values).map(([name, value]) =>
    ({ name, value }))),
  set: settings => writeSettings(settings)
});
class OosSettingObserverBase {
  constructor(id, session) { this.id = id; this.session = session; }
  callback(_setting) { return Promise.resolve(); }
}
const mozSettings = eventTarget({
  onsettingchange: null,
  createLock: () => ({
    get: name => promiseRequest(readSetting(name).then(value =>
      ({ [String(name)]: value }))),
    set: values => promiseRequest(writeSettings(values)),
    clear: name => promiseRequest(name === undefined
      ? systemCall('settings', 'clear')
      : systemCall('settings', 'remove', { name: String(name) }))
  }),
  addObserver: (name, observer) => settingsService.addObserver(name, observer),
  removeObserver: (name, observer) =>
    settingsService.removeObserver(name, observer)
});

const alarmFromRecord = record => ({ id: record.id,
  date: new Date(record.value.dateMs),
  respectTimezone: record.value.ignoreTimezone
    ? 'ignoreTimezone' : 'honorTimezone',
  ignoreTimezone: !!record.value.ignoreTimezone,
  data: record.value.data });
const alarmManager = Object.freeze({
  getAll: () => systemCall('alarms', 'list').then(records =>
    records.map(alarmFromRecord)),
  add: options => {
    if (!options || !(options.date instanceof Date) ||
        !Number.isFinite(options.date.getTime()))
      return Promise.reject(new TypeError('Alarm date must be valid'));
    return systemCall('alarms', 'add', { dateMs: options.date.getTime(),
      ignoreTimezone: !!options.ignoreTimezone,
      data: encodedValue(options.data) });
  },
  remove: id => systemCall('alarms', 'remove', { id: Number(id) })
});
const mozAlarms = Object.freeze({
  getAll: () => promiseRequest(alarmManager.getAll()),
  add: (date, respectTimezone, data) => promiseRequest(alarmManager.add({
    date, ignoreTimezone: respectTimezone === 'ignoreTimezone', data
  })),
  remove: id => { alarmManager.remove(id).catch(() => {}); }
});

const systemMessageHandlers = new Map();
const internalSystemMessageHandlers = new Map();
const explicitSystemSubscriptions = new Set();
const pendingSystemMessages = new Map();
let systemMessageCursor = 0;
let systemMessagePollTimer = 0;
let systemMessagePolling = false;
const systemMessageData = entry => ({
  json: () => entry.payload,
  webActivityRequestHandler: () => {
    throw notSupported('WebActivityRequestHandler');
  }
});
const deliverSystemMessage = entry => {
  for (const handler of internalSystemMessageHandlers.get(entry.topic) || []) {
    try { handler(entry.payload); } catch (_) {}
  }
  const handler = systemMessageHandlers.get(entry.topic);
  if (typeof handler === 'function') {
    defer(() => handler(entry.payload));
  } else {
    const pending = pendingSystemMessages.get(entry.topic) || [];
    pending.push(entry);
    pendingSystemMessages.set(entry.topic, pending);
  }
  const event = typeof Event === 'function' ? new Event('systemmessage')
    : { type: 'systemmessage' };
  try {
    Object.defineProperties(event, {
      name: { value: entry.topic },
      data: { value: systemMessageData(entry) }
    });
    globalThis.dispatchEvent?.(event);
  } catch (_) {
    if (typeof globalThis.onsystemmessage === 'function')
      globalThis.onsystemmessage({ type: 'systemmessage', name: entry.topic,
        data: systemMessageData(entry) });
  }
};
const scheduleSystemMessagePoll = delay => {
  if (systemMessagePollTimer ||
      (!systemMessageHandlers.size && !internalSystemMessageHandlers.size &&
       !explicitSystemSubscriptions.size))
    return;
  systemMessagePollTimer = setTimeout(() => {
    systemMessagePollTimer = 0;
    pollSystemMessages();
  }, delay);
};
const pollSystemMessages = async () => {
  if (systemMessagePolling) return;
  systemMessagePolling = true;
  try {
    const entries = await systemCall('system-messages', 'poll', {
      after: systemMessageCursor, limit: 32
    });
    for (const entry of entries) {
      systemMessageCursor = Math.max(systemMessageCursor, entry.sequence);
      deliverSystemMessage(entry);
    }
    scheduleSystemMessagePoll(entries.length ? 50 : 1000);
  } catch (_) {
    scheduleSystemMessagePoll(2000);
  } finally {
    systemMessagePolling = false;
  }
};
const subscribeSystemMessage = (topic, activate = false) => {
  topic = String(topic);
  if (activate) explicitSystemSubscriptions.add(topic);
  return systemCall('system-messages', 'subscribe', { topic }).then(() => {
    scheduleSystemMessagePoll(0);
  });
};
const addInternalSystemMessageHandler = (topic, handler) => {
  const handlers = internalSystemMessageHandlers.get(topic) || [];
  if (!handlers.includes(handler)) handlers.push(handler);
  internalSystemMessageHandlers.set(topic, handlers);
  return subscribeSystemMessage(topic);
};
const setSystemMessageHandler = (topic, handler) => {
  topic = String(topic);
  if (handler === null || handler === undefined) {
    systemMessageHandlers.delete(topic);
    return;
  }
  if (typeof handler !== 'function')
    throw new TypeError('System message handler must be a function');
  systemMessageHandlers.set(topic, handler);
  subscribeSystemMessage(topic).catch(() => {});
  for (const entry of pendingSystemMessages.get(topic) || [])
    defer(() => handler(entry.payload));
  pendingSystemMessages.delete(topic);
};
const hasPendingSystemMessage = topic => {
  pollSystemMessages();
  return (pendingSystemMessages.get(String(topic)) || []).length > 0;
};
const systemMessageManager = Object.freeze({
  subscribe: topic => subscribeSystemMessage(topic, true)
});
for (const permission of runtime.permissions) {
  if (permission.startsWith('system-message:'))
    subscribeSystemMessage(permission.slice(15)).catch(() => {});
}
if (globalThis.ServiceWorkerRegistration?.prototype) {
  try {
    Object.defineProperty(globalThis.ServiceWorkerRegistration.prototype,
      'systemMessageManager', { configurable: true,
        get: () => systemMessageManager });
  } catch (_) {}
}

class OosWebActivity {
  constructor(name, data = {}) {
    if (!name) throw new TypeError('WebActivity requires a name');
    this.name = String(name); this.data = data; this.id = 0;
    this.started = false; this.cancelled = false;
  }
  async start() {
    if (this.started)
      throw new DOMException('Activity already started', 'InvalidStateError');
    this.started = true;
    this.id = await systemCall('activities', 'start', { name: this.name,
      data: encodedValue(this.data) });
    while (!this.cancelled) {
      const state = await systemCall('activities', 'status', { id: this.id });
      if (!state) throw new DOMException('Activity was cancelled', 'AbortError');
      if (state.state === 'resolved') return state.result;
      if (state.state === 'rejected') throw state.error;
      await new Promise(resolve => setTimeout(resolve, 250));
    }
    throw new DOMException('Activity was cancelled', 'AbortError');
  }
  cancel() {
    if (!this.started || this.cancelled) return;
    this.cancelled = true;
    systemCall('activities', 'cancel', { id: this.id }).catch(() => {});
  }
}
class OosMozActivity {
  constructor(options = {}) {
    const activity = new OosWebActivity(options.name, options.data);
    const request = promiseRequest(activity.start());
    request.cancel = () => activity.cancel();
    return request;
  }
}

const contactKind = value => value?._oosKind || 'contact';
const cleanContact = record => {
  const value = { ...record.value };
  delete value._oosKind;
  value.id = String(record.id);
  return value;
};
const contactRecords = () => systemCall('contacts', 'list');
const recordsByKind = kind => contactRecords().then(records =>
  records.filter(record => contactKind(record.value) === kind));
const addContactRecord = (kind, value) => systemCall('contacts', 'add', {
  value: encodedValue({ ...value, _oosKind: kind })
});
const updateContactRecord = (kind, id, value) =>
  systemCall('contacts', 'put', { id: Number(id),
    value: encodedValue({ ...value, id: undefined, _oosKind: kind }) });
const removeContactRecord = id => systemCall('contacts', 'remove', {
  id: Number(id)
});
const contactSearchText = contact => JSON.stringify(contact).toLocaleLowerCase();
const filterContacts = (contacts, options = {}) => {
  const needle = String(options.filterValue || '').toLocaleLowerCase();
  if (!needle) return contacts;
  return contacts.filter(contact => contactSearchText(contact).includes(needle));
};
const contactCursor = (promise, batchSize = 20) => {
  let index = 0;
  const records = Promise.resolve(promise);
  return { async next() {
    const values = await records;
    if (index >= values.length) return [];
    const batch = values.slice(index, index + Math.max(1, Number(batchSize) || 20));
    index += batch.length;
    return batch;
  } };
};
const domCursor = promise => {
  let index = 0;
  let pending = false;
  const cursor = { result: null, error: null, done: false,
    onsuccess: null, onerror: null,
    continue() {
      if (!pending && !cursor.done) { pending = true; defer(emit); }
    }
  };
  const emit = async () => {
    pending = false;
    try {
      const values = await promise;
      cursor.result = index < values.length ? values[index++] : null;
      cursor.done = cursor.result === null;
      cursor.onsuccess?.call(cursor, { type: 'success', target: cursor });
    } catch (error) {
      cursor.error = error; cursor.done = true;
      cursor.onerror?.call(cursor, { type: 'error', target: cursor });
    }
  };
  pending = true; defer(emit); return cursor;
};
const emitContactsChange = (reason, contacts) => contactsService.dispatchEvent({
  type: contactsService.CONTACTS_CHANGE_EVENT, target: contactsService,
  reason, contacts
});
const metadataValues = kind => recordsByKind(kind).then(records =>
  records.map(record => ({ ...record.value, id: String(record.id),
    _oosKind: undefined })));
const removeMetadataBy = async (kind, predicate) => {
  const records = await recordsByKind(kind);
  for (const record of records) {
    if (predicate(record.value, record.id)) await removeContactRecord(record.id);
  }
};
const contactsService = eventTarget({
  service_id: 'contacts', CONTACTS_CHANGE_EVENT: 'contactschange',
  BLOCKEDNUMBER_CHANGE_EVENT: 'blockednumberchange',
  GROUP_CHANGE_EVENT: 'groupchange', SIM_CONTACT_LOADED_EVENT: 'simcontactloaded',
  SPEEDDIAL_CHANGE_EVENT: 'speeddialchange',
  async add(contacts) {
    for (const contact of contacts || []) {
      const id = await addContactRecord('contact', contact);
      contact.id = String(id);
    }
    emitContactsChange('CREATE', contacts || []);
  },
  async update(contacts) {
    for (const contact of contacts || []) {
      if (!contact.id) throw new TypeError('Contact id is required');
      await updateContactRecord('contact', contact.id, contact);
    }
    emitContactsChange('UPDATE', contacts || []);
  },
  async remove(ids) {
    const removed = [];
    for (const id of ids || []) {
      const record = await systemCall('contacts', 'get', { id: Number(id) });
      if (record) removed.push({ ...record, id: String(id) });
      await removeContactRecord(id);
    }
    emitContactsChange('REMOVE', removed);
  },
  async clearContacts() {
    const records = await recordsByKind('contact');
    for (const record of records) await removeContactRecord(record.id);
    emitContactsChange('REMOVE', records.map(cleanContact));
  },
  get: (id, _onlyMainData) => systemCall('contacts', 'get', {
    id: Number(id)
  }).then(value => value ? { ...value, id: String(id), _oosKind: undefined }
    : null),
  getAll: (options, batchSize, _onlyMainData) => Promise.resolve(contactCursor(
    recordsByKind('contact').then(records =>
      filterContacts(records.map(cleanContact), options)), batchSize)),
  find: (options, batchSize) => Promise.resolve(contactCursor(
    recordsByKind('contact').then(records =>
      filterContacts(records.map(cleanContact), options)), batchSize)),
  getCount: () => recordsByKind('contact').then(records => records.length),
  matches: (filterBy, filter, value) => recordsByKind('contact').then(records => {
    const needle = String(value || '').toLocaleLowerCase();
    const fields = Array.isArray(filterBy) ? filterBy : [filterBy];
    return records.some(record => fields.some(field => {
      const candidate = record.value?.[String(field).toLowerCase()] ??
        record.value?.[field];
      const text = JSON.stringify(candidate || '').toLocaleLowerCase();
      return String(filter).toUpperCase() === 'EQUALS'
        ? text === JSON.stringify(needle) : text.includes(needle);
    }));
  }),
  async importVcf(vcf) {
    const blocks = String(vcf).split(/END:VCARD/i);
    let imported = 0;
    for (const block of blocks) {
      if (!/BEGIN:VCARD/i.test(block)) continue;
      const contact = { name: [], tel: [], email: [] };
      for (const line of block.split(/\r?\n/)) {
        const separator = line.indexOf(':');
        if (separator < 0) continue;
        const key = line.slice(0, separator).split(';')[0].toUpperCase();
        const value = line.slice(separator + 1).trim();
        if (key === 'FN') contact.name.push(value);
        else if (key === 'TEL') contact.tel.push({ value });
        else if (key === 'EMAIL') contact.email.push({ value });
      }
      await addContactRecord('contact', contact); imported += 1;
    }
    return imported;
  },
  async addGroup(name) { return addContactRecord('group', { name: String(name) }); },
  getAllGroups: () => metadataValues('group'),
  async updateGroup(id, name) {
    return updateContactRecord('group', id, { name: String(name) });
  },
  removeGroup: id => removeContactRecord(id),
  getContactidsFromGroup: groupId => recordsByKind('contact').then(records =>
    records.filter(record => (record.value.groups || []).map(String)
      .includes(String(groupId))).map(record => String(record.id))),
  async addSpeedDial(dialKey, tel, contactId) {
    await removeMetadataBy('speed-dial', value =>
      String(value.dialKey) === String(dialKey));
    return addContactRecord('speed-dial', { dialKey: String(dialKey),
      tel: String(tel), contactId: String(contactId) });
  },
  getSpeedDials: () => metadataValues('speed-dial'),
  updateSpeedDial(dialKey, tel, contactId) {
    return this.addSpeedDial(dialKey, tel, contactId);
  },
  removeSpeedDial: dialKey => removeMetadataBy('speed-dial', value =>
    String(value.dialKey) === String(dialKey)),
  async addBlockedNumber(number) {
    return addContactRecord('blocked-number', { number: String(number) });
  },
  getAllBlockedNumbers: () => metadataValues('blocked-number').then(values =>
    values.map(value => value.number)),
  findBlockedNumbers: options => metadataValues('blocked-number').then(values =>
    values.map(value => value.number).filter(number =>
      String(number).includes(String(options?.filterValue || '')))),
  removeBlockedNumber: number => removeMetadataBy('blocked-number', value =>
    String(value.number) === String(number)),
  async setIce(contactId, position) {
    await removeMetadataBy('ice', value =>
      String(value.contactId) === String(contactId));
    return addContactRecord('ice', { contactId: String(contactId),
      position: Number(position) });
  },
  getAllIce: () => metadataValues('ice'),
  removeIce: contactId => removeMetadataBy('ice', value =>
    String(value.contactId) === String(contactId))
});
const mozContacts = eventTarget({
  oncontactchange: null,
  find: options => promiseRequest(recordsByKind('contact').then(records =>
    filterContacts(records.map(cleanContact), options))),
  getAll: options => domCursor(recordsByKind('contact').then(records =>
    filterContacts(records.map(cleanContact), options))),
  getCount: () => promiseRequest(contactsService.getCount()),
  save: contact => promiseRequest((async () => {
    if (contact.id) await contactsService.update([contact]);
    else await contactsService.add([contact]);
    return contact.id;
  })()),
  remove: contact => promiseRequest(contactsService.remove([
    typeof contact === 'object' ? contact.id : contact
  ])),
  clear: () => promiseRequest(contactsService.clearContacts())
});
let contactsEventsSubscribed = false;
const ensureContactsEvents = () => {
  if (contactsEventsSubscribed) return Promise.resolve();
  contactsEventsSubscribed = true;
  return addInternalSystemMessageHandler('contacts', payload => {
    if (payload?.sourceApp === runtime.appId) return;
    contactsService.dispatchEvent({ type: contactsService.CONTACTS_CHANGE_EVENT,
      target: contactsService, reason: String(payload?.operation || '')
        .toUpperCase(), contacts: null });
    mozContacts.dispatchEvent({ type: 'contactchange', target: mozContacts,
      reason: payload?.operation, contactID: payload?.id });
  });
};
const mozContactsAddEventListener = mozContacts.addEventListener;
mozContacts.addEventListener = (type, callback) => {
  if (type === 'contactchange') ensureContactsEvents().catch(() => {});
  mozContactsAddEventListener(type, callback);
};
const contactsServiceAddEventListener = contactsService.addEventListener;
contactsService.addEventListener = (type, callback) => {
  if (type === contactsService.CONTACTS_CHANGE_EVENT)
    ensureContactsEvents().catch(() => {});
  contactsServiceAddEventListener(type, callback);
};
const speedDialManager = Object.freeze({
  get: dialKey => promiseRequest(contactsService.getSpeedDials().then(values =>
    values.find(value => String(value.dialKey) === String(dialKey)) || null)),
  set: (dialKey, tel, contactId = '') => promiseRequest(
    contactsService.addSpeedDial(dialKey, tel, contactId)),
  remove: dialKey => promiseRequest(contactsService.removeSpeedDial(dialKey))
});

const appObject = app => ({ name: app.name, manifestUrl:
  `${globalThis.location.protocol}//${app.id}.localhost/manifest.webmanifest`,
  manifestURL:
  `${globalThis.location.protocol}//${app.id}.localhost/manifest.webmanifest`,
  installState: 'INSTALLED', removable: false,
  status: app.enabled ? 'ENABLED' : 'DISABLED', updateManifestUrl: '',
  updateState: 'IDLE', updateUrl: '', allowedAutoDownload: false,
  preloaded: true, id: app.id, version: app.version, runtime: app.runtime });
const unsupportedAppOperation = name => () => failedPromise(`AppsManager.${name}`);
const appsService = eventTarget({
  service_id: 'apps', APP_DOWNLOAD_FAILED_EVENT: 'appdownloadfailed',
  APP_INSTALLED_EVENT: 'appinstalled', APP_INSTALLING_EVENT: 'appinstalling',
  APP_UNINSTALLED_EVENT: 'appuninstalled',
  APP_UPDATE_AVAILABLE_EVENT: 'appupdateavailable',
  APP_UPDATED_EVENT: 'appupdated', APP_UPDATING_EVENT: 'appupdating',
  APPSTATUS_CHANGED_EVENT: 'appstatuschanged',
  getAll: () => systemCall('applications', 'list').then(apps =>
    apps.map(appObject)),
  getApp: manifestUrl => systemCall('applications', 'list').then(apps => {
    const text = String(manifestUrl);
    const app = apps.find(candidate => text.includes(candidate.id));
    if (!app) throw new DOMException('Application was not found', 'NotFoundError');
    return appObject(app);
  }),
  getState: () => Promise.resolve('RUNNING'),
  getUpdatePolicy: () => Promise.resolve({ enabled: false,
    connType: 'WI_FI_ONLY', delay: 0 }),
  cancelDownload: unsupportedAppOperation('cancelDownload'),
  checkForUpdate: unsupportedAppOperation('checkForUpdate'),
  clear: unsupportedAppOperation('clear'),
  installPackage: unsupportedAppOperation('installPackage'),
  installPwa: unsupportedAppOperation('installPwa'),
  setEnabled: unsupportedAppOperation('setEnabled'),
  setTokenProvider: unsupportedAppOperation('setTokenProvider'),
  setUpdatePolicy: unsupportedAppOperation('setUpdatePolicy'),
  uninstall: unsupportedAppOperation('uninstall'),
  update: unsupportedAppOperation('update'),
  verify: unsupportedAppOperation('verify')
});

const audioPolicyRequest = action => systemCall('audio-policy', 'request', {
  action
});
const audioVolumeService = eventTarget({
  service_id: 'audiovolumemanager', AUDIO_VOLUME_CHANGED_EVENT:
    'audiovolumechanged',
  requestVolumeUp: () => audioPolicyRequest('VOLUME_UP'),
  requestVolumeDown: () => audioPolicyRequest('VOLUME_DOWN'),
  requestVolumeShow: () => audioPolicyRequest('VOLUME_SHOW')
});
const volumeManager = Object.freeze({
  requestUp: () => { audioVolumeService.requestVolumeUp().catch(() => {}); },
  requestDown: () => { audioVolumeService.requestVolumeDown().catch(() => {}); },
  requestShow: () => { audioVolumeService.requestVolumeShow().catch(() => {}); }
});
const audioChannelManager = eventTarget({
  headphones: false, telephonyChannelActive: false,
  contentChannelActive: false, notificationChannelActive: false,
  alarmChannelActive: false, publicnotificationChannelActive: false,
  ringerChannelActive: false, normalChannelActive: false,
  onheadphoneschange: null, getVolumeControlChannel: () => 'normal'
});
class OosAudioChannelClient {
  constructor(channel = 'normal') {
    this.channel = String(channel); this.isActive = false;
    this.onactivestatechanged = null; eventTarget(this);
  }
  requestChannel() {
    this.isActive = true;
    return systemCall('audio-policy', 'set', { name: `channel:${this.channel}`,
      value: encodedValue({ active: true, requestedAt: Date.now() }) })
      .then(() => { this.dispatchEvent({ type: 'activestatechanged',
        target: this }); return true; });
  }
  abandonChannel() {
    this.isActive = false;
    return systemCall('audio-policy', 'set', { name: `channel:${this.channel}`,
      value: encodedValue({ active: false, requestedAt: Date.now() }) })
      .then(() => { this.dispatchEvent({ type: 'activestatechanged',
        target: this }); return true; });
  }
}
class OosSpeakerManager {
  constructor() {
    this.speakerforced = false; this.forcespeaker = false;
    this.onspeakerforcedchange = null; eventTarget(this);
  }
}

const inputMethod = {
  setComposition: text => systemCall('input-method', 'set', {
    name: 'composition', value: encodedValue(String(text))
  }).then(() => true),
  endComposition: (text = '') => systemCall('input-method', 'set', {
    name: 'composition', value: encodedValue(String(text))
  }).then(() => true),
  sendKey: key => Promise.all([inputMethod.keydown(key),
    inputMethod.keyup(key)]).then(() => true),
  keydown: key => {
    const target = document.activeElement || document.body;
    return Promise.resolve(target.dispatchEvent(new KeyboardEvent('keydown', {
      key: String(key), bubbles: true, cancelable: true })));
  },
  keyup: key => {
    const target = document.activeElement || document.body;
    return Promise.resolve(target.dispatchEvent(new KeyboardEvent('keyup', {
      key: String(key), bubbles: true, cancelable: true })));
  },
  deleteBackward: () => {
    const target = document.activeElement;
    if (target && typeof target.setRangeText === 'function') {
      const start = Math.max(0, target.selectionStart - 1);
      target.setRangeText('', start, target.selectionStart, 'end');
      target.dispatchEvent(new Event('input', { bubbles: true }));
      return Promise.resolve(true);
    }
    return Promise.resolve(false);
  },
  setSelectedOption: index => systemCall('input-method', 'set', {
    name: 'selected-option', value: encodedValue(Number(index))
  }),
  setSelectedOptions: indexes => systemCall('input-method', 'set', {
    name: 'selected-options', value: encodedValue(Array.from(indexes, Number))
  }),
  removeFocus: () => { document.activeElement?.blur?.(); }
};
const timeService = Object.freeze({
  service_id: 'time',
  getTime: () => systemCall('time', 'get').then(value => value.timeMs),
  setTime: value => systemCall('time', 'set', { name: 'requested-time-ms',
    value: encodedValue(Number(value instanceof Date ? value.getTime() : value)) }),
  setTimezone: value => systemCall('time', 'set', {
    name: 'requested-timezone', value: encodedValue(String(value))
  })
});
const powerManagerService = Object.freeze({
  service_id: 'powermanager',
  cpuSleepAllowed: () => Promise.resolve(true),
  extScreenBrightness: () => Promise.resolve(0),
  extScreenEnabled: () => Promise.resolve(false),
  factoryReset: () => Promise.resolve('NORMAL'),
  keyLightBrightness: () => Promise.resolve(0),
  keyLightEnabled: () => Promise.resolve(false),
  screenBrightness: () => Promise.resolve(100),
  screenEnabled: () => Promise.resolve(true),
  controlScreen: () => failedPromise('PowerManager.controlScreen'),
  powerOff: () => failedPromise('PowerManager.powerOff'),
  reboot: () => failedPromise('PowerManager.reboot')
});

const activeNotifications = new Map();
class OosNotification {
  constructor(title, options = {}) {
    if (!notificationsAllowed)
      throw new DOMException('Notification permission denied', 'NotAllowedError');
    ensureNotificationEvents().catch(() => {});
    this.title = String(title); this.body = String(options.body || '');
    this.tag = String(options.tag || ''); this.icon = options.icon || '';
    this.data = options.data; this.dir = options.dir || 'auto';
    this.lang = options.lang || ''; this.onclick = null; this.onshow = null;
    this.onerror = null; this.onclose = null; this._closed = false;
    eventTarget(this);
    this._stored = systemCall('notifications', 'add', {
      value: encodedValue({ title: this.title, body: this.body, tag: this.tag,
        icon: this.icon, data: this.data, createdAt: Date.now() })
    }).then(id => {
      this.id = Number(id); activeNotifications.set(this.id, this);
      this.dispatchEvent({ type: 'show', target: this }); return this.id;
    }, error => { this.dispatchEvent({ type: 'error', target: this, error });
      throw error; });
  }
  close() {
    if (this._closed) return;
    this._closed = true;
    this._stored.then(id => systemCall('notifications', 'remove', { id }))
      .catch(() => {});
    if (this.id) activeNotifications.delete(this.id);
    defer(() => this.dispatchEvent({ type: 'close', target: this }));
  }
  static requestPermission(callback) {
    const result = Promise.resolve(notificationsAllowed ? 'granted' : 'denied');
    if (typeof callback === 'function') result.then(callback);
    return result;
  }
  static get permission() { return notificationsAllowed ? 'granted' : 'denied'; }
}
let notificationEventsSubscribed = false;
const ensureNotificationEvents = () => {
  if (notificationEventsSubscribed) return Promise.resolve();
  notificationEventsSubscribed = true;
  return addInternalSystemMessageHandler('notification', payload => {
    const notification = activeNotifications.get(Number(payload?.id));
    if (!notification) return;
    if (payload.action === 'close') notification.close();
    else notification.dispatchEvent({ type: 'click', target: notification,
      action: payload.action || 'default' });
  });
};

const unsupportedPromiseApi = (name, methods) => {
  const api = {};
  for (const method of methods)
    api[method] = () => failedPromise(`${name}.${method}`);
  return eventTarget(api);
};
const dataCallManager = unsupportedPromiseApi('DataCallManager',
  ['requestDataCall', 'getDataCallState']);
const telephonyService = unsupportedPromiseApi('Telephony',
  ['dial', 'dialEmergency', 'hangUpAll', 'startTone', 'stopTone']);
const externalApi = unsupportedPromiseApi('Externalapi',
  ['invoke', 'call', 'get']);
const fmRadio = eventTarget({ enabled: false, frequency: null,
  enable: () => failedPromise('FmRadio.enable'),
  disable: () => failedPromise('FmRadio.disable'),
  setFrequency: () => failedPromise('FmRadio.setFrequency'),
  seekUp: () => failedPromise('FmRadio.seekUp'),
  seekDown: () => failedPromise('FmRadio.seekDown'),
  cancelSeek: () => failedPromise('FmRadio.cancelSeek')
});
const networkStats = {
  getAvailableNetworks: () => failedRequest('NetworkStats.getAvailableNetworks'),
  getSamples: () => failedRequest('NetworkStats.getSamples'),
  clearStats: () => failedRequest('NetworkStats.clearStats'),
  clearAllStats: () => failedRequest('NetworkStats.clearAllStats'),
  addAlarm: () => failedRequest('NetworkStats.addAlarm'),
  getAllAlarms: () => failedRequest('NetworkStats.getAllAlarms'),
  removeAlarms: () => failedRequest('NetworkStats.removeAlarms')
};
const nfcManager = unsupportedPromiseApi('NFC',
  ['startPoll', 'stopPoll', 'powerOff', 'sendFile']);
const seManager = unsupportedPromiseApi('SEManager', ['getSEReaders']);
const tcpSocketFactory = Object.freeze({
  open: () => { throw notSupported('MozTCPSocket.open'); }
});
class OosTcpSocket {
  constructor() { throw notSupported('TcpSocket'); }
}

let largeTextEnabled = false;
systemCall('accessibility', 'get', { name: 'large-text-enabled' })
  .then(value => {
    if (!value.found || largeTextEnabled === !!value.value) return;
    largeTextEnabled = !!value.value;
    globalThis.dispatchEvent?.(new Event('largetextenabledchanged'));
  }).catch(() => {});
let largeTextEventsSubscribed = false;
const ensureLargeTextEvents = () => {
  if (largeTextEventsSubscribed) return Promise.resolve();
  largeTextEventsSubscribed = true;
  return addInternalSystemMessageHandler(
    'accessibility:large-text-enabled', payload => {
      if (payload?.name !== 'large-text-enabled' ||
          largeTextEnabled === !!payload.value) return;
      largeTextEnabled = !!payload.value;
      globalThis.dispatchEvent?.(new Event('largetextenabledchanged'));
    });
};
const nativeWindowAddEventListener = globalThis.addEventListener;
try {
  Object.defineProperty(globalThis, 'addEventListener', { configurable: true,
    writable: true, value(type, callback, options) {
      if (type === 'largetextenabledchanged')
        ensureLargeTextEvents().catch(() => {});
      return nativeWindowAddEventListener.call(this, type, callback, options);
    } });
} catch (_) {}
const virtualCursor = eventTarget({
  enabled: false, position: { x: 0, y: 0 },
  setEnabled(value) { this.enabled = !!value; return Promise.resolve(); },
  move(direction) {
    const key = { up: 'ArrowUp', down: 'ArrowDown', left: 'ArrowLeft',
      right: 'ArrowRight' }[String(direction).toLowerCase()];
    return key ? inputMethod.sendKey(key) : Promise.resolve(false);
  },
  click: () => inputMethod.sendKey('Enter')
});
try {
  Object.defineProperty(globalThis, 'WebActivity', { value: OosWebActivity,
    writable: true, configurable: true });
  Object.defineProperty(globalThis, 'Notification', { value: OosNotification,
    writable: true, configurable: true });
} catch (_) {}
define(globalThis, 'MozActivity', OosMozActivity);
define(globalThis, 'MozSpeakerManager', OosSpeakerManager);

const appRecord = Object.freeze({
  manifestURL: `${globalThis.location.origin}/manifest.webapp`,
  origin: globalThis.location.origin,
  installOrigin: `${globalThis.location.protocol}//system.localhost${
    globalThis.location.port ? `:${globalThis.location.port}` : ''}`
});
if (runtime.apiProfile === 'kaios-v3') {
  define(globalThis, 'lib_session', { Session: OosDaemonSession });
  define(globalThis, 'lib_devicecapability', {
    DeviceCapabilityManager: Object.freeze({
      get: _session => Promise.resolve(deviceCapabilityService)
    })
  });
  if (settingsAllowed) define(globalThis, 'lib_settings', {
    SettingObserverBase: OosSettingObserverBase,
    SettingsManager: Object.freeze({
      get: _session => Promise.resolve(settingsService)
    })
  });
  if (contactsAllowed) define(globalThis, 'lib_contacts', {
    ContactsManager: Object.freeze({
      get: _session => Promise.resolve(contactsService)
    })
  });
  if (applicationsAllowed) define(globalThis, 'lib_apps', {
    AppsManager: Object.freeze({
      get: _session => Promise.resolve(appsService)
    })
  });
  if (audioPolicyAllowed) define(globalThis, 'lib_audiovolume', {
    AudioVolumeManager: Object.freeze({
      get: _session => Promise.resolve(audioVolumeService)
    })
  });
  if (powerAllowed) define(globalThis, 'lib_powermanager', {
    PowermanagerService: Object.freeze({
      get: _session => Promise.resolve(powerManagerService)
    })
  });
  if (timeAllowed) {
    const timeLibrary = { TimeService: Object.freeze({
      get: _session => Promise.resolve(timeService)
    }) };
    define(globalThis, 'lib_time', timeLibrary);
    define(globalThis, 'lib_timeservice', timeLibrary);
  }
  if (tcpSocketAllowed) define(globalThis, 'lib_tcpsocket', {
    TcpSocket: Object.freeze({
      get: _session => failedPromise('TcpSocket service')
    })
  });
  if (telephonyAllowed) define(globalThis, 'lib_telephony', {
    Telephony: Object.freeze({
      get: _session => Promise.resolve(telephonyService)
    })
  });
  if (tcpSocketAllowed) {
    define(nav, 'mozTCPSocket', tcpSocketFactory);
    define(globalThis, 'TcpSocket', OosTcpSocket);
  }
  const b2g = {};
  if (alarmsAllowed) b2g.alarmManager = alarmManager;
  if (storageAllowed) Object.assign(b2g, { getDeviceStorage,
    getDeviceStorages });
  if (powerSupplyAllowed) b2g.powerSupplyManager = batteryManager;
  if (bluetoothAllowed) b2g.bluetooth = bluetoothManager;
  if (cameraAllowed) b2g.cameras = camerasManager;
  if (modemAllowed) {
    b2g.mobileConnection = [mobileConnection];
    b2g.dataCallManager = dataCallManager;
  }
  if (audioPolicyAllowed) {
    b2g.audioChannelManager = audioChannelManager;
    b2g.AudioChannelClient = OosAudioChannelClient;
  }
  if (inputMethodAllowed) b2g.inputMethod = inputMethod;
  if (externalApiAllowed) b2g.externalapi = externalApi;
  if (fmRadioAllowed) b2g.fmRadio = fmRadio;
  b2g.virtualCursor = virtualCursor;
  if (typeof globalThis.AudioContext === 'function')
    b2g.AudioContext = globalThis.AudioContext;
  if (typeof globalThis.HTMLMediaElement === 'function')
    b2g.HTMLMediaElement = globalThis.HTMLMediaElement;
  define(nav, 'b2g', b2g);
} else {
  define(nav, 'getFeature', getFeature);
  define(nav, 'hasFeature', hasFeature);
  const mozApps = { getSelf: () => domRequest(appRecord) };
  if (applicationsAllowed) mozApps.mgmt = {
    getAll: () => promiseRequest(appsService.getAll()),
    uninstall: () => failedRequest('Apps.mgmt.uninstall'),
    applyDownload: () => failedRequest('Apps.mgmt.applyDownload')
  };
  define(nav, 'mozApps', mozApps);
  define(nav, 'mozSetMessageHandler', setSystemMessageHandler);
  define(nav, 'mozHasPendingMessage', hasPendingSystemMessage);
  if (alarmsAllowed) define(nav, 'mozAlarms', mozAlarms);
  try { Object.defineProperty(nav, 'largeTextEnabled', { configurable: true,
    enumerable: true, get: () => largeTextEnabled }); } catch (_) {}
  define(nav, 'volumeManager', volumeManager);
  if (settingsAllowed) define(nav, 'mozSettings', mozSettings);
  if (contactsAllowed) {
    define(nav, 'mozContacts', mozContacts);
    define(nav, 'mozSpeedDial', speedDialManager);
  }
  if (ownedDataStoreDefinitions.size)
    define(nav, 'getDataStores', getDataStores);
  if (storageAllowed) {
    define(nav, 'getDeviceStorage', getDeviceStorage);
    define(nav, 'getDeviceStorages', getDeviceStorages);
  }
  if (powerAllowed) {
    define(nav, 'requestWakeLock', makeWakeLock);
    define(nav, 'mozPower', { requestWakeLock: makeWakeLock });
  }
  define(nav, 'mozBattery', batteryManager);
  if (powerSupplyAllowed) define(nav, 'powersupply', batteryManager);
  if (wifiAllowed) define(nav, 'mozWifiManager', wifiManager);
  if (bluetoothAllowed) define(nav, 'mozBluetooth', bluetoothManager);
  if (cameraAllowed) define(nav, 'mozCameras', camerasManager);
  if (inputMethodAllowed) define(nav, 'mozInputMethod', inputMethod);
  if (networkStatsAllowed) define(nav, 'mozNetworkStats', networkStats);
  if (nfcAllowed) define(nav, 'mozNfc', nfcManager);
  if (secureElementAllowed) define(nav, 'seManager', seManager);
  if (fmRadioAllowed) define(nav, 'mozFMRadio', fmRadio);
  if (tcpSocketAllowed) define(nav, 'mozTCPSocket', tcpSocketFactory);
  define(nav, 'spatialNavigationEnabled', true);
  if (modemAllowed) {
    define(nav, 'mozMobileConnection', mobileConnection);
    define(nav, 'mozMobileConnections', [mobileConnection]);
  }
}
})();
)JS";
  WebKitUserScript *user_script = webkit_user_script_new(
      script_.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
  if (!user_script) {
    error_ = "cannot create KaiOS document-start script";
    return false;
  }
  webkit_user_content_manager_add_script(manager_, user_script);
  webkit_user_script_unref(user_script);
  return true;
}

void KaiOsApiBridge::handleLifecycle(WebKitUserContentManager *,
                                     JSCValue *message, void *data) {
  auto *bridge = static_cast<KaiOsApiBridge *>(data);
  if (!message || !jsc_value_is_string(message))
    return;
  char *value = jsc_value_to_string(message);
  const bool close = value && std::strcmp(value, "close") == 0;
  if (value && std::strncmp(value, "trace:", 6) == 0)
    std::fprintf(stderr, "WPE DOM %s\n", value + 6);
  g_free(value);
  if (close && bridge->close_callback_)
    bridge->close_callback_(bridge->close_context_);
}

int KaiOsApiBridge::handleDeviceApi(WebKitUserContentManager *,
                                    JSCValue *message,
                                    WebKitScriptMessageReply *reply,
                                    void *data) {
  auto *bridge = static_cast<KaiOsApiBridge *>(data);
  if (!message || !jsc_value_is_string(message)) {
    webkit_script_message_reply_return_error_message(reply,
                                                     "invalid OOS API request");
    return TRUE;
  }
  char *encoded = jsc_value_to_string(message);
  oos::apps::JsonValue request;
  std::string parse_error;
  const bool parsed = encoded &&
                      oos::apps::parseJson(encoded, request, parse_error) &&
                      request.isObject();
  g_free(encoded);
  if (!parsed) {
    webkit_script_message_reply_return_error_message(
        reply,
        parse_error.empty() ? "invalid OOS API request" : parse_error.c_str());
    return TRUE;
  }

  const oos::apps::JsonValue *operation = request.get("operation");
  const oos::apps::JsonValue *volume = request.get("volume");
  const oos::apps::JsonValue *path = request.get("path");
  const oos::apps::JsonValue *data_value = request.get("data");
  const oos::apps::JsonValue *mode = request.get("mode");
  if (!operation || !operation->isString() || !volume || !volume->isNumber() ||
      volume->integerValue() < 0 ||
      volume->integerValue() > OOS_DEVICE_API_REMOVABLE) {
    webkit_script_message_reply_return_error_message(
        reply, "invalid OOS API arguments");
    return TRUE;
  }
  uint16_t request_operation = 0;
  if (operation->stringValue() == "list")
    request_operation = OOS_DEVICE_API_LIST_FILES;
  else if (operation->stringValue() == "read")
    request_operation = OOS_DEVICE_API_READ_FILE;
  else if (operation->stringValue() == "write")
    request_operation = OOS_DEVICE_API_WRITE_FILE;
  else if (operation->stringValue() == "delete")
    request_operation = OOS_DEVICE_API_DELETE_FILE;
  else if (operation->stringValue() == "free-space")
    request_operation = OOS_DEVICE_API_FREE_SPACE;
  else if (operation->stringValue() == "used-space")
    request_operation = OOS_DEVICE_API_USED_SPACE;
  else if (operation->stringValue() == "platform")
    request_operation = OOS_DEVICE_API_PLATFORM_CALL;
  else {
    webkit_script_message_reply_return_error_message(
        reply, "unknown OOS API operation");
    return TRUE;
  }
  const char *request_path = "";
  if (request_operation == OOS_DEVICE_API_READ_FILE ||
      request_operation == OOS_DEVICE_API_WRITE_FILE ||
      request_operation == OOS_DEVICE_API_DELETE_FILE ||
      request_operation == OOS_DEVICE_API_PLATFORM_CALL) {
    if (!path || !path->isString() || path->stringValue().empty()) {
      webkit_script_message_reply_return_error_message(
          reply, request_operation == OOS_DEVICE_API_PLATFORM_CALL
                     ? "platform method is required"
                     : "device storage path is required");
      return TRUE;
    }
    request_path = path->stringValue().c_str();
  }

  uint16_t request_flags = 0;
  gsize request_payload_size = 0;
  guchar *request_payload = nullptr;
  if (request_operation == OOS_DEVICE_API_PLATFORM_CALL) {
    if (!data_value || !data_value->isString() ||
        data_value->stringValue().size() > 256 * 1024) {
      webkit_script_message_reply_return_error_message(
          reply, "platform arguments must be a bounded JSON string");
      return TRUE;
    }
    request_payload_size = data_value->stringValue().size();
    request_payload = reinterpret_cast<guchar *>(
        g_memdup2(data_value->stringValue().data(), request_payload_size));
  } else if (request_operation == OOS_DEVICE_API_WRITE_FILE) {
    if (!data_value || !data_value->isString() || !mode || !mode->isString()) {
      webkit_script_message_reply_return_error_message(
          reply, "device storage write data and mode are required");
      return TRUE;
    }
    if (mode->stringValue() == "create")
      request_flags = OOS_DEVICE_API_WRITE_CREATE;
    else if (mode->stringValue() == "replace")
      request_flags = OOS_DEVICE_API_WRITE_REPLACE;
    else if (mode->stringValue() == "append")
      request_flags = OOS_DEVICE_API_WRITE_APPEND;
    else {
      webkit_script_message_reply_return_error_message(
          reply, "invalid device storage write mode");
      return TRUE;
    }
    request_payload = g_base64_decode(data_value->stringValue().c_str(),
                                      &request_payload_size);
    if (request_payload_size > 64u * 1024u * 1024u) {
      g_free(request_payload);
      webkit_script_message_reply_return_error_message(
          reply, "device storage write exceeds 64 MiB");
      return TRUE;
    }
  }

  const bool queued =
      bridge->api_client_ &&
      bridge->api_client_->enqueue(
          request_operation, static_cast<uint16_t>(volume->integerValue()),
          request_flags, request_path, request_payload,
          static_cast<uint32_t>(request_payload_size), message, reply);
  g_free(request_payload);
  if (!queued) {
    webkit_script_message_reply_return_error_message(
        reply, "OOS device API client is busy or shutting down");
    return TRUE;
  }
  return TRUE;
}

} // namespace oos::web
