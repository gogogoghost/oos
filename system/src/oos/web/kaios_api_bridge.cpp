#include "oos/web/kaios_api_bridge.h"

#include "oos/apps/json.h"
#include "oos/web/device_api_transport.h"

#include <glib-object.h>
#include <jsc/jsc.h>
#include <wpe/webkit.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <utility>

namespace oos::web {

KaiOsApiBridge::KaiOsApiBridge(std::string app_id, std::string api_profile,
                               int api_fd)
    : app_id_(std::move(app_id)), api_profile_(std::move(api_profile)),
      api_fd_(api_fd) {}

KaiOsApiBridge::~KaiOsApiBridge() {
  if (manager_) {
    webkit_user_content_manager_unregister_script_message_handler(
        manager_, "oosLifecycle", nullptr);
    webkit_user_content_manager_unregister_script_message_handler(
        manager_, "oosDeviceApi", nullptr);
    g_object_unref(manager_);
  }
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

  script_ = "(() => { 'use strict';"
            "const runtime = Object.freeze({appId:'" +
            app_id_ + "',apiProfile:'" + api_profile_ +
            "',bridgeVersion:2,traceKeys:" +
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
  for (const type of ['keydown', 'keyup'])
    globalThis.addEventListener(type, event =>
      trace(`${type}:key=${event.key}:code=${event.code}:repeat=${event.repeat}`),
      true);
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
const deviceHandler = globalThis.webkit?.messageHandlers?.oosDeviceApi;
const deviceCall = (operation, volume, path = '') => {
  if (!deviceHandler)
    return Promise.reject(notSupported('OOS device API bridge'));
  return deviceHandler.postMessage(JSON.stringify({ operation, volume, path }));
};
const decodeBase64 = encoded => {
  const binary = globalThis.atob(encoded);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; ++index)
    bytes[index] = binary.charCodeAt(index);
  return bytes;
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
const makeDeviceCursor = (volume, prefix = '') => {
  const files = listDeviceFiles(volume).then(entries => prefix
    ? entries.filter(file => file.name.startsWith(prefix)) : entries);
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
const storage = (storageName, volume, isRemovable, isDefault) => eventTarget({
  storageName, storagePath: storageName, isRemovable, default: isDefault,
  canBeMounted: false, canBeFormatted: false, lowDiskSpace: false,
  onchange: null,
  add: () => failedRequest('DeviceStorage.add'),
  addNamed: () => failedRequest('DeviceStorage.addNamed'),
  appendNamed: () => failedRequest('DeviceStorage.appendNamed'),
  delete: () => failedRequest('DeviceStorage.delete'),
  get: path => promiseRequest(readDeviceFile({ volume, path }).then(bytes =>
    makeDeviceFile(volume, { path, size: bytes.length,
      lastModified: Date.now() }, bytes))),
  getEditable: () => failedRequest('DeviceStorage.getEditable'),
  getRoot: () => failedRequest('DeviceStorage.getRoot'),
  enumerate: prefix => makeDeviceCursor(volume, prefix || ''),
  enumerateEditable: prefix => makeDeviceCursor(volume, prefix || ''),
  available: () => domRequest('available'),
  storageStatus: () => domRequest('available'),
  freeSpace: () => domRequest(0),
  usedSpace: () => domRequest(0),
  format: () => failedRequest('DeviceStorage.format'),
  mount: () => failedRequest('DeviceStorage.mount'),
  unmount: () => failedRequest('DeviceStorage.unmount')
});
const internalStorage = storage('sdcard', 0, false, true);
const removableStorage = storage('sdcard1', 1, true, false);
const getDeviceStorages = name => name === 'sdcard'
  ? [internalStorage, removableStorage] : [storage(name, 0, false, true)];
const getDeviceStorage = name => getDeviceStorages(name)[0];
const makeManager = (initial = {}) => new Proxy(eventTarget(initial), {
  get(target, property) {
    if (property in target) return target[property];
    if (typeof property === 'symbol' || property === 'then') return undefined;
    if (String(property).startsWith('on')) return null;
    return () => null;
  }
});
const lock = topic => ({ topic, released: false, unlock() {
  this.released = true;
}, release() { this.unlock(); } });
define(nav, 'requestWakeLock', topic => lock(topic));
define(nav, 'wakeLock', { request: async topic => lock(topic) });
define(nav, 'getDeviceStorage', getDeviceStorage);
define(nav, 'getDeviceStorages', getDeviceStorages);
define(nav, 'getDataStores', async () => []);
define(nav, 'getFeature', async () => undefined);
define(nav, 'hasFeature', async () => false);
define(nav, 'mozHasPendingMessage', () => false);
const systemMessageHandlers = new Map();
define(nav, 'mozSetMessageHandler', (name, callback) => {
  if (typeof callback === 'function') systemMessageHandlers.set(name, callback);
});
define(nav, 'vibrate', () => false);
const appRecord = Object.freeze({
  manifestURL: `${globalThis.location.origin}/manifest.webmanifest`,
  origin: globalThis.location.origin,
  installOrigin: 'http://system.localhost'
});
define(nav, 'mozApps', makeManager({ getSelf: () => domRequest(appRecord),
  getInstalled: () => domRequest([appRecord]), mgmt: makeManager() }));
define(nav, 'mozContacts', makeManager({ find: () => domRequest([]),
  getAll: () => emptyCursor(), save: () => failedRequest('Contacts.save'),
  remove: () => failedRequest('Contacts.remove'), clear: () => failedRequest('Contacts.clear') }));
define(nav, 'mozSettings', makeManager({ createLock: () => makeManager({
  get: () => domRequest({}), set: () => domRequest(null),
  clear: () => domRequest(null) }) }));
define(nav, 'mozPower', makeManager());
define(nav, 'volumeManager', makeManager({ requestVolume: () => domRequest(null) }));
define(nav, 'powersupply', makeManager());
define(nav, 'mozTCPSocket', makeManager({ open: () => null }));
class OosWebActivity {
  constructor(name, data = {}) { this.source = { name, data }; this.started = false; }
  start() {
    if (this.started) return Promise.reject(notSupported('WebActivity.start'));
    this.started = true;
    return Promise.reject(notSupported('WebActivity.start'));
  }
  cancel() {}
}
define(globalThis, 'WebActivity', OosWebActivity);
define(globalThis, 'MozActivity', OosWebActivity);

const managers = {
  alarmManager: makeManager({ add: () => domRequest(null),
    remove: () => domRequest(null), getAll: () => domRequest([]) }),
  audioChannelManager: makeManager({ volumeControlChannel: 'normal' }),
  bluetooth: makeManager({ enabled: false, defaultAdapter: null }),
  cameras: makeManager({ getListOfCameras: () => [] }),
  dataCallManager: makeManager(), externalapi: makeManager(),
  fmRadio: makeManager({ enabled: false, frequency: null }),
  inputMethod: makeManager(), mobileConnection: makeManager(),
  virtualCursor: makeManager({ enabled: false }),
  powerManager: nav.mozPower, settings: nav.mozSettings
};
if (runtime.apiProfile === 'kaios-v3') {
  const b2g = makeManager({ ...managers, getDeviceStorage,
    getDeviceStorages, AudioChannelClient: makeManager() });
  if (typeof globalThis.AudioContext === 'function')
    b2g.AudioContext = globalThis.AudioContext;
  if (typeof globalThis.HTMLMediaElement === 'function')
    b2g.HTMLMediaElement = globalThis.HTMLMediaElement;
  define(nav, 'b2g', b2g);
} else {
  define(nav, 'mozAlarms', managers.alarmManager);
  define(nav, 'mozAudioChannelManager', managers.audioChannelManager);
  define(nav, 'mozBluetooth', managers.bluetooth);
  define(nav, 'mozCameras', managers.cameras);
  define(nav, 'dataCallManager', managers.dataCallManager);
  define(nav, 'externalapi', managers.externalapi);
  define(nav, 'mozFMRadio', managers.fmRadio);
  define(nav, 'mozInputMethod', managers.inputMethod);
  define(nav, 'mozMobileConnection', managers.mobileConnection);
  define(nav, 'spatialNavigationEnabled', false);
}
})();
)JS";
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
  else {
    webkit_script_message_reply_return_error_message(
        reply, "unknown OOS API operation");
    return TRUE;
  }
  const char *request_path = "";
  if (request_operation == OOS_DEVICE_API_READ_FILE) {
    if (!path || !path->isString() || path->stringValue().empty()) {
      webkit_script_message_reply_return_error_message(
          reply, "device storage path is required");
      return TRUE;
    }
    request_path = path->stringValue().c_str();
  }

  void *payload = nullptr;
  uint32_t payload_size = 0;
  const int result =
      oos_device_api_request(bridge->api_fd_, request_operation,
                             static_cast<uint16_t>(volume->integerValue()),
                             request_path, &payload, &payload_size, 30000);
  if (result != 0) {
    webkit_script_message_reply_return_error_message(
        reply, result < 0 ? std::strerror(-result) : "OOS device API failed");
    return TRUE;
  }

  char *response = nullptr;
  if (request_operation == OOS_DEVICE_API_LIST_FILES) {
    response = static_cast<char *>(g_malloc(payload_size + 1));
    if (payload_size)
      std::memcpy(response, payload, payload_size);
    response[payload_size] = '\0';
  } else {
    response = reinterpret_cast<char *>(
        g_base64_encode(static_cast<const guchar *>(payload), payload_size));
  }
  oos_device_api_free(payload);
  if (!response) {
    webkit_script_message_reply_return_error_message(reply,
                                                     "encode OOS API response");
    return TRUE;
  }
  JSCValue *value =
      jsc_value_new_string(jsc_value_get_context(message), response);
  g_free(response);
  webkit_script_message_reply_return_value(reply, value);
  g_object_unref(value);
  return TRUE;
}

} // namespace oos::web
