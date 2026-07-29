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
      : socket_fd_(socket_fd), worker_(&DeviceApiClient::run, this) {}

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
    std::unique_ptr<Completion> completion(
        static_cast<Completion *>(data));
    if (completion->result != 0) {
      webkit_script_message_reply_return_error_message(
          completion->reply,
          completion->result < 0 ? std::strerror(-completion->result)
                                 : "OOS device API failed");
    } else {
      JSCValue *value =
          jsc_value_new_string(completion->context,
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
      g_main_context_invoke(nullptr, finish, completion.release());
    }
  }

  int socket_fd_ = -1;
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
            "const runtime = Object.freeze({appId:" + jsonString(app_id_) +
            ",apiProfile:" + jsonString(api_profile_) +
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
  enabled: true, connection: { status: 'disconnected', network: null },
  connectionInformation: { ipAddress: '', signalStrength: 0,
    relSignalStrength: 0, linkSpeed: 0 },
  onstatuschange: null, onconnectioninfoupdate: null,
  getNetworks: () => domRequest(platformCall('wifi.scan').then(networks =>
    networks.map(network => {
      const flags = String(network.capabilities || '');
      const security = /WPA-EAP/i.test(flags) ? ['WPA-EAP']
        : /WPA/i.test(flags) ? ['WPA-PSK']
        : /WEP/i.test(flags) ? ['WEP'] : [];
      return { ...network, signalStrength: network.signalDbm,
        relSignalStrength: Math.max(0, Math.min(100,
          2 * (network.signalDbm + 100))), security,
        capabilities: /WPS/i.test(flags) ? ['WPS'] : [],
        connected: false, known: false };
    }))),
  getKnownNetworks: () => domRequest(platformCall('wifi.networks').then(
    networks => networks.map(network => ({ ...network, known: true,
      connected: /CURRENT/i.test(String(network.capabilities || '')) })))),
  associate: network => domRequest(platformCall('wifi.connect', {
    ssid: String(network?.ssid || ''),
    security: /WPA/i.test(String(network?.capabilities ||
      network?.security?.join?.('') || '')) ? 1 : 0,
    credential: String(network?.psk || network?.password || '')
  })),
  forget: network => domRequest(platformCall('wifi.forget', {
    networkId: Number(network?.networkId ?? network?.id ?? -1)
  })),
  disconnect: () => domRequest(platformCall('wifi.disconnect')),
  reconnect: () => domRequest(platformCall('wifi.reconnect')),
  setStaticIpMode: (_network, info) => domRequest(info?.enabled
    ? platformCall('ip.static', {
        interfaceName: String(info.ifname || 'wlan0'),
        address: String(info.ipaddr || ''),
        prefixLength: Number(info.maskLength || 0),
        gateway: String(info.gateway || ''),
        dns1: String(info.dns1 || ''), dns2: String(info.dns2 || '')
      })
    : platformCall('ip.dhcp', { timeoutMs: 15000 })),
  async refresh() {
    const status = await platformCall('wifi.status');
    const state = String(status.state).toUpperCase();
    this.connection.status = state === 'COMPLETED' ? 'connected'
      : state === 'ASSOCIATING' ? 'connecting'
      : state === 'ASSOCIATED' ? 'associated' : 'disconnected';
    this.connection.network = status.ssid ? { ssid: status.ssid,
      bssid: status.bssid, networkId: status.networkId } : null;
    this.connectionInformation.ipAddress = status.ipAddress;
    this.dispatchEvent({ type: 'statuschange', target: this });
    return status;
  },
  getIpConfiguration: () => platformCall('ip.status'),
  useDhcp: timeoutMs => platformCall('ip.dhcp', { timeoutMs })
});
const bluetoothAdapter = eventTarget({
  state: 'disabled', discovering: false, ondevicefound: null,
  enable: () => domRequest(platformCall('bluetooth.enable').then(() => {
    bluetoothAdapter.state = 'enabled'; return true;
  })),
  disable: () => domRequest(platformCall('bluetooth.disable').then(() => {
    bluetoothAdapter.state = 'disabled'; return true;
  })),
  startDiscovery: () => domRequest((async () => {
    bluetoothAdapter.discovering = true;
    const devices = await platformCall('bluetooth.classic-scan');
    for (const device of devices)
      bluetoothAdapter.dispatchEvent({ type: 'devicefound', target:
        bluetoothAdapter, device });
    bluetoothAdapter.discovering = false;
    return devices;
  })()),
  stopDiscovery: () => domRequest(Promise.resolve().then(() => {
    bluetoothAdapter.discovering = false; return true;
  })),
  pair: (address, transport = 0) => domRequest(platformCall('bluetooth.pair',
    { address: String(address), transport: Number(transport) || 0 })),
  unpair: address => domRequest(platformCall('bluetooth.unpair',
    { address: String(address) })),
  cancelPairing: address => domRequest(platformCall('bluetooth.cancel-pairing',
    { address: String(address) })),
  leScan: durationMs => platformCall('bluetooth.le-scan', { durationMs })
});
const bluetoothManager = eventTarget({ enabled: false,
  defaultAdapter: bluetoothAdapter, onenabled: null, ondisabled: null });

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
  async refresh() {
    const snapshot = await platformCall('modem.snapshot');
    this.radioState = String(snapshot.radioState);
    this.cardState = String(snapshot.sim.cardState);
    this.lastKnownNetwork = snapshot.networkOperator.numeric || null;
    this.lastKnownHomeNetwork = this.lastKnownNetwork;
    this.voice = { connected: snapshot.voiceRegistration.state === 1,
      state: snapshot.voiceRegistration.state,
      type: snapshot.voiceRegistration.radioTechnology,
      network: snapshot.networkOperator, signalStrength:
        snapshot.signal.lteStrength };
    this.data = { connected: snapshot.dataRegistration.state === 1,
      state: snapshot.dataRegistration.state,
      type: snapshot.dataRegistration.radioTechnology,
      network: snapshot.networkOperator, signalStrength:
        snapshot.signal.lteStrength };
    this.dispatchEvent({ type: 'voicechange', target: this });
    this.dispatchEvent({ type: 'datachange', target: this });
    return snapshot;
  },
  setRadioEnabled: enabled => domRequest(platformCall('modem.radio-power',
    { enabled: !!enabled }))
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
  const b2g = {};
  if (storageAllowed) Object.assign(b2g, { getDeviceStorage,
    getDeviceStorages });
  if (powerSupplyAllowed) b2g.powerSupplyManager = batteryManager;
  if (bluetoothAllowed) b2g.bluetooth = bluetoothManager;
  if (cameraAllowed) b2g.cameras = camerasManager;
  if (modemAllowed) b2g.mobileConnection = [mobileConnection];
  if (typeof globalThis.AudioContext === 'function')
    b2g.AudioContext = globalThis.AudioContext;
  if (typeof globalThis.HTMLMediaElement === 'function')
    b2g.HTMLMediaElement = globalThis.HTMLMediaElement;
  define(nav, 'b2g', b2g);
} else {
  define(nav, 'getFeature', getFeature);
  define(nav, 'hasFeature', hasFeature);
  define(nav, 'mozApps', { getSelf: () => domRequest(appRecord) });
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

  const bool queued = bridge->api_client_ && bridge->api_client_->enqueue(
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
