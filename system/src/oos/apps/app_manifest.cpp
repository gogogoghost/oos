#include "oos/apps/app_manifest.h"

#include "oos/apps/json.h"
#include "oos/apps/zip_archive.h"

#include <cctype>
#include <limits>

namespace oos::apps {
namespace {

bool requiredString(const JsonValue &root, const char *key, std::string &value,
                    std::string &error) {
  const JsonValue *field = root.get(key);
  if (!field || !field->isString() || field->stringValue().empty()) {
    error = std::string("manifest field '") + key + "' must be a string";
    return false;
  }
  value = field->stringValue();
  return true;
}

bool optionalString(const JsonValue &root, const char *key, std::string &value,
                    std::string &error) {
  const JsonValue *field = root.get(key);
  if (!field)
    return true;
  if (!field->isString()) {
    error = std::string("manifest field '") + key + "' must be a string";
    return false;
  }
  value = field->stringValue();
  return true;
}

bool validIdentifier(const std::string &value) {
  if (value.empty() || value.size() > 128 || value.front() == '.' ||
      value.back() == '.')
    return false;
  bool last_dot = false;
  for (const unsigned char character : value) {
    if (character == '.') {
      if (last_dot)
        return false;
      last_dot = true;
      continue;
    }
    last_dot = false;
    if (!std::isalnum(character) && character != '-' && character != '_')
      return false;
  }
  return true;
}

bool validVersion(const std::string &value) {
  if (value.empty() || value.size() > 64)
    return false;
  for (const unsigned char character : value) {
    if (!std::isalnum(character) && character != '.' && character != '-' &&
        character != '+')
      return false;
  }
  return true;
}

bool boundedMemory(const JsonValue &root, AppManifest &manifest,
                   std::string &error) {
  const JsonValue *memory = root.get("memory");
  if (!memory)
    return true;
  if (!memory->isObject()) {
    error = "manifest field 'memory' must be an object";
    return false;
  }
  const JsonValue *stack = memory->get("stack_bytes");
  const JsonValue *heap = memory->get("heap_bytes");
  if ((stack && (!stack->isNumber() || stack->integerValue() < 64 * 1024 ||
                 stack->integerValue() > 1024 * 1024)) ||
      (heap && (!heap->isNumber() || heap->integerValue() < 1024 * 1024 ||
                heap->integerValue() > 16 * 1024 * 1024))) {
    error = "manifest memory limits are outside the supported range";
    return false;
  }
  if (stack)
    manifest.stack_bytes = static_cast<uint32_t>(stack->integerValue());
  if (heap)
    manifest.heap_bytes = static_cast<uint32_t>(heap->integerValue());
  return true;
}

bool collectPermissions(const JsonValue *permissions, AppManifest &manifest,
                        std::string &error) {
  if (!permissions)
    return true;
  if (!permissions->isObject()) {
    error = "manifest field 'permissions' must be an object";
    return false;
  }
  for (const auto &entry : permissions->objectValue()) {
    if (entry.first.empty() || entry.first.size() > 128) {
      error = "manifest permission name is empty or too long";
      return false;
    }
    manifest.requested_permissions.push_back(entry.first);
    if (!entry.second.isObject())
      continue;
    const JsonValue *access = entry.second.get("access");
    if (!access)
      continue;
    if (!access->isString()) {
      error = "manifest permission access must be a string";
      return false;
    }
    if (access->stringValue() == "readonly") {
      manifest.requested_permissions.push_back(entry.first + ":read");
    } else if (access->stringValue() == "readwrite") {
      manifest.requested_permissions.push_back(entry.first + ":read");
      manifest.requested_permissions.push_back(entry.first + ":write");
    } else if (access->stringValue() == "createonly") {
      manifest.requested_permissions.push_back(entry.first + ":create");
    }
  }
  return true;
}

bool collectMessages(const JsonValue *messages, AppManifest &manifest,
                     std::string &error) {
  if (!messages)
    return true;
  if (!messages->isArray()) {
    error = "manifest field 'messages' must be an array";
    return false;
  }
  for (const JsonValue &message : messages->arrayValue()) {
    if (message.isString()) {
      if (message.stringValue().empty() || message.stringValue().size() > 128) {
        error = "manifest system message name is empty or too long";
        return false;
      }
      manifest.requested_permissions.push_back("system-message:" +
                                                message.stringValue());
      continue;
    }
    if (!message.isObject() || message.objectValue().empty()) {
      error = "manifest field 'messages' contains an invalid entry";
      return false;
    }
    for (const auto &entry : message.objectValue()) {
      if (entry.first.empty() || entry.first.size() > 128) {
        error = "manifest system message name is empty or too long";
        return false;
      }
      manifest.requested_permissions.push_back("system-message:" +
                                                entry.first);
    }
  }
  return true;
}

bool collectActivities(const JsonValue *activities, AppManifest &manifest,
                       std::string &error) {
  if (!activities)
    return true;
  if (!activities->isObject()) {
    error = "manifest field 'activities' must be an object";
    return false;
  }
  for (const auto &entry : activities->objectValue()) {
    if (entry.first.empty() || entry.first.size() > 128 ||
        !entry.second.isObject()) {
      error = "manifest field 'activities' contains an invalid handler";
      return false;
    }
    manifest.handlers.push_back({"activity", entry.first});
  }
  return true;
}

bool collectDataStores(const JsonValue *stores, const char *kind,
                       AppManifest &manifest, std::string &error) {
  if (!stores)
    return true;
  if (!stores->isObject()) {
    error = std::string("manifest field '") + kind + "' must be an object";
    return false;
  }
  for (const auto &entry : stores->objectValue()) {
    if (entry.first.empty() || entry.first.size() > 128 ||
        !entry.second.isObject()) {
      error = std::string("manifest field '") + kind +
              "' contains an invalid store";
      return false;
    }
    const JsonValue *access = entry.second.get("access");
    if (!access || !access->isString() ||
        (access->stringValue() != "readonly" &&
         access->stringValue() != "readwrite")) {
      error = std::string("manifest field '") + kind +
              "' requires readonly or readwrite access";
      return false;
    }
    manifest.requested_permissions.emplace_back(
        std::string(kind) + ":" + access->stringValue() + ":" + entry.first);
  }
  return true;
}

} // namespace

const char *packageKindName(PackageKind kind) {
  switch (kind) {
  case PackageKind::OosWasmV1:
    return "oos-wasm-v1";
  case PackageKind::KaiOs25:
    return "kaios-2.5";
  case PackageKind::KaiOs3:
    return "kaios-3";
  }
  return "unknown";
}

const char *runtimeKindName(RuntimeKind kind) {
  return kind == RuntimeKind::Wamr ? "wamr" : "wpe";
}

bool parseAppManifest(const std::string &json, AppManifest &manifest,
                      std::string &error) {
  JsonValue root;
  if (!parseJson(json, root, error)) {
    error = "parse oos-manifest.json: " + error;
    return false;
  }
  if (!root.isObject()) {
    error = "oos-manifest.json must contain an object";
    return false;
  }
  const JsonValue *format = root.get("format");
  if (!format || !format->isNumber() || format->integerValue() != 1) {
    error = "unsupported or missing app package format";
    return false;
  }

  AppManifest parsed;
  parsed.format = 1;
  std::string package_kind;
  std::string runtime_kind;
  if (!requiredString(root, "id", parsed.id, error) ||
      !requiredString(root, "name", parsed.name, error) ||
      !requiredString(root, "version", parsed.version, error) ||
      !requiredString(root, "package_kind", package_kind, error) ||
      !requiredString(root, "runtime_kind", runtime_kind, error) ||
      !requiredString(root, "api_profile", parsed.api_profile, error) ||
      !requiredString(root, "entrypoint", parsed.entrypoint, error) ||
      !optionalString(root, "fallback_entrypoint", parsed.fallback_entrypoint,
                      error) ||
      !optionalString(root, "role", parsed.role, error) ||
      !boundedMemory(root, parsed, error) ||
      !collectPermissions(root.get("permissions"), parsed, error))
    return false;

  if (!validIdentifier(parsed.id) || !validVersion(parsed.version)) {
    error = "manifest id or version contains unsupported characters";
    return false;
  }
  if (package_kind == "oos-wasm-v1")
    parsed.package_kind = PackageKind::OosWasmV1;
  else if (package_kind == "kaios-2.5")
    parsed.package_kind = PackageKind::KaiOs25;
  else if (package_kind == "kaios-3")
    parsed.package_kind = PackageKind::KaiOs3;
  else {
    error = "unsupported package_kind: " + package_kind;
    return false;
  }
  if (runtime_kind == "wamr")
    parsed.runtime_kind = RuntimeKind::Wamr;
  else if (runtime_kind == "wpe")
    parsed.runtime_kind = RuntimeKind::Wpe;
  else {
    error = "unsupported runtime_kind: " + runtime_kind;
    return false;
  }
  if ((parsed.package_kind == PackageKind::OosWasmV1) !=
      (parsed.runtime_kind == RuntimeKind::Wamr)) {
    error = "package_kind and runtime_kind do not match";
    return false;
  }
  if (!validPackagePath(parsed.entrypoint) || parsed.entrypoint.back() == '/' ||
      (!parsed.fallback_entrypoint.empty() &&
       (!validPackagePath(parsed.fallback_entrypoint) ||
        parsed.fallback_entrypoint.back() == '/'))) {
    error = "manifest entrypoint is not a safe package path";
    return false;
  }
  manifest = std::move(parsed);
  return true;
}

bool parseKaiOsManifest(const std::string &json, PackageKind kind,
                        const std::string &app_id, AppManifest &manifest,
                        std::string &error) {
  if (kind != PackageKind::KaiOs25 && kind != PackageKind::KaiOs3) {
    error = "invalid KaiOS package kind";
    return false;
  }
  JsonValue root;
  if (!parseJson(json, root, error) || !root.isObject()) {
    error = "parse KaiOS manifest: " + error;
    return false;
  }
  AppManifest parsed;
  parsed.format = 1;
  parsed.id = app_id;
  parsed.package_kind = kind;
  parsed.runtime_kind = RuntimeKind::Wpe;
  parsed.api_profile =
      kind == PackageKind::KaiOs25 ? "kaios-b2g48" : "kaios-v3";
  if (!validIdentifier(parsed.id) ||
      !requiredString(root, "name", parsed.name, error)) {
    if (error.empty())
      error = "KaiOS package requires a valid explicit application id";
    return false;
  }
  const JsonValue *features = root.get("b2g_features");
  const JsonValue *version =
      kind == PackageKind::KaiOs3 && features && features->isObject()
          ? features->get("version")
          : root.get("version");
  parsed.version =
      version && version->isString() ? version->stringValue() : "0";
  if (!validVersion(parsed.version)) {
    error = "KaiOS manifest version contains unsupported characters";
    return false;
  }
  const char *entry_name =
      kind == PackageKind::KaiOs25 ? "launch_path" : "start_url";
  const JsonValue *entrypoint = root.get(entry_name);
  parsed.entrypoint = entrypoint && entrypoint->isString()
                          ? entrypoint->stringValue()
                          : "index.html";
  while (!parsed.entrypoint.empty() && parsed.entrypoint.front() == '/')
    parsed.entrypoint.erase(parsed.entrypoint.begin());
  while (parsed.entrypoint.rfind("./", 0) == 0)
    parsed.entrypoint.erase(0, 2);
  const size_t query = parsed.entrypoint.find_first_of("?#");
  if (query != std::string::npos)
    parsed.entrypoint.resize(query);
  if (!validPackagePath(parsed.entrypoint) || parsed.entrypoint.back() == '/') {
    error = "KaiOS manifest start path is not a package path";
    return false;
  }
  const JsonValue *role = root.get("role");
  const JsonValue *permissions = root.get("permissions");
  const JsonValue *messages = root.get("messages");
  const JsonValue *activities = root.get("activities");
  if (kind == PackageKind::KaiOs3 && features && features->isObject()) {
    role = features->get("role");
    permissions = features->get("permissions");
    if (features->get("messages"))
      messages = features->get("messages");
    if (features->get("activities"))
      activities = features->get("activities");
  }
  if (role && role->isString())
    parsed.role = role->stringValue();
  if (!collectPermissions(permissions, parsed, error) ||
      !collectMessages(messages, parsed, error) ||
      !collectActivities(activities, parsed, error))
    return false;
  if (kind == PackageKind::KaiOs25 &&
      (!collectDataStores(root.get("datastores-owned"), "datastore-owned",
                          parsed, error) ||
       !collectDataStores(root.get("datastores-access"), "datastore-access",
                          parsed, error)))
    return false;
  manifest = std::move(parsed);
  return true;
}

} // namespace oos::apps
