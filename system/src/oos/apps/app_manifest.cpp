#include "oos/apps/app_manifest.h"

#include "oos/apps/json.h"
#include "oos/apps/wasm_artifact.h"

#include <cctype>
#include <initializer_list>
#include <set>
#include <string_view>

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

bool onlyFields(const JsonValue &object,
                std::initializer_list<std::string_view> allowed,
                std::string_view context, std::string &error) {
  for (const auto &field : object.objectValue()) {
    bool known = false;
    for (std::string_view name : allowed) {
      if (field.first == name) {
        known = true;
        break;
      }
    }
    if (!known) {
      error = std::string(context) + " contains unknown field '" + field.first +
              "'";
      return false;
    }
  }
  return true;
}

bool validIdentifier(const std::string &value) {
  if (value.empty() || value.size() > 128)
    return false;

  size_t separators = 0;
  bool segment_start = true;
  bool hyphen = false;
  for (const unsigned char character : value) {
    if (character == '.') {
      if (segment_start || hyphen)
        return false;
      ++separators;
      segment_start = true;
      hyphen = false;
      continue;
    }
    if (segment_start) {
      if (character < 'a' || character > 'z')
        return false;
      segment_start = false;
      hyphen = false;
      continue;
    }
    if (character == '-') {
      hyphen = true;
      continue;
    }
    if ((character < 'a' || character > 'z') &&
        (character < '0' || character > '9'))
      return false;
    hyphen = false;
  }
  return !segment_start && !hyphen && separators >= 2;
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

bool validPackagePath(const std::string &value, const char *prefix) {
  if (value.empty() || value.size() > 512 || value.back() == '/' ||
      value.rfind(prefix, 0) != 0)
    return false;
  size_t segment_start = 0;
  while (segment_start < value.size()) {
    const size_t separator = value.find('/', segment_start);
    const size_t segment_end =
        separator == std::string::npos ? value.size() : separator;
    const std::string segment =
        value.substr(segment_start, segment_end - segment_start);
    if (segment.empty() || segment == "." || segment == "..")
      return false;
    for (const unsigned char character : segment) {
      if (!(std::isalnum(character) || character == '-' || character == '_' ||
            character == '.'))
        return false;
    }
    if (separator == std::string::npos)
      break;
    segment_start = separator + 1;
  }
  return true;
}

bool validPackageFile(const std::string &value, const char *prefix,
                      const char *extension) {
  const size_t extension_length = std::char_traits<char>::length(extension);
  return validPackagePath(value, prefix) && value.size() > extension_length &&
         value.compare(value.size() - extension_length, extension_length,
                       extension) == 0;
}

bool validWasmBasePath(const std::string &value, const char *prefix) {
  return validPackagePath(value, prefix) && isSuffixlessWasmBasePath(value);
}

bool parseRuntime(const JsonValue &object, AppRuntimeKind &runtime,
                  std::string &error) {
  std::string value;
  if (!requiredString(object, "runtime", value, error))
    return false;
  if (value == "js") {
    runtime = AppRuntimeKind::JavaScript;
    return true;
  }
  if (value == "wasm") {
    runtime = AppRuntimeKind::WebAssembly;
    return true;
  }
  error = "manifest runtime must be 'js' or 'wasm'";
  return false;
}

bool parseExecutable(const JsonValue &object, const char *path_prefix,
                     AppEntrypoint &entry, std::string &error,
                     bool module_declaration = false) {
  if (!object.isObject()) {
    error = "manifest field 'entry' must be an object";
    return false;
  }
  if (!onlyFields(
          object,
          module_declaration
              ? std::initializer_list<std::string_view>{"name", "runtime",
                                                        "path"}
              : std::initializer_list<std::string_view>{"runtime", "path"},
          module_declaration ? "manifest module" : "manifest entry", error))
    return false;
  if (!parseRuntime(object, entry.runtime, error))
    return false;
  if (!requiredString(object, "path", entry.path, error))
    return false;
  if (entry.runtime == AppRuntimeKind::JavaScript) {
    if (!(validPackageFile(entry.path, path_prefix, ".js") ||
          validPackageFile(entry.path, path_prefix, ".mjs"))) {
      error = "JavaScript path must be a package-relative .js or .mjs file";
      return false;
    }
    return true;
  }
  if (!validWasmBasePath(entry.path, path_prefix)) {
    error = "Wasm path must be a package-relative base path without a suffix";
    return false;
  }
  return true;
}

bool validModuleName(const std::string &name) {
  if (name.empty() || name.size() > 128)
    return false;
  for (const unsigned char character : name) {
    if (!(std::isalnum(character) || character == '-' || character == '_' ||
          character == '.'))
      return false;
  }
  return name != "." && name != "..";
}

bool collectModules(const JsonValue *modules, AppManifest &manifest,
                    std::string &error) {
  if (!modules)
    return true;
  if (!modules->isArray()) {
    error = "manifest field 'modules' must be an array";
    return false;
  }
  std::set<std::string> names;
  for (const JsonValue &value : modules->arrayValue()) {
    if (!value.isObject()) {
      error = "manifest module must be an object";
      return false;
    }
    AppModule module;
    if (!requiredString(value, "name", module.name, error) ||
        !validModuleName(module.name) || !names.insert(module.name).second) {
      error = "manifest module name is invalid or duplicated";
      return false;
    }
    AppEntrypoint executable;
    if (!parseExecutable(value, kModulePrefix, executable, error, true)) {
      error = "module '" + module.name + "': " + error;
      return false;
    }
    module.runtime = executable.runtime;
    module.path = std::move(executable.path);
    manifest.modules.push_back(std::move(module));
  }
  return true;
}

bool collectHandlers(const JsonValue *handlers, AppManifest &manifest,
                     std::string &error) {
  if (!handlers)
    return true;
  if (!handlers->isArray()) {
    error = "manifest field 'handlers' must be an array";
    return false;
  }
  for (const JsonValue &value : handlers->arrayValue()) {
    AppHandler handler;
    if (!value.isObject() ||
        !onlyFields(value, {"kind", "value"}, "manifest handler", error) ||
        !requiredString(value, "kind", handler.kind, error) ||
        !requiredString(value, "value", handler.value, error) ||
        handler.kind.size() > 128 || handler.value.size() > 1024) {
      error = "manifest handler must contain bounded kind and value strings";
      return false;
    }
    manifest.handlers.push_back(std::move(handler));
  }
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
    if (!entry.second.isObject() ||
        !onlyFields(entry.second, {"access"}, "manifest permission", error)) {
      error = "manifest permission '" + entry.first +
              "' must be an object containing only access";
      return false;
    }
    manifest.requested_permissions.push_back(entry.first);
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
    } else {
      error = "manifest permission access must be readonly, readwrite, or "
              "createonly";
      return false;
    }
  }
  return true;
}

bool rejectLegacyFields(const JsonValue &root, std::string &error) {
  constexpr const char *legacy_fields[] = {
      "format",     "package_kind",        "runtime_kind", "api_profile",
      "entrypoint", "fallback_entrypoint", "memory",       "ui",
  };
  for (const char *field : legacy_fields) {
    if (root.get(field)) {
      error = std::string("manifest field '") + field +
              "' is obsolete in the fixed OOS package format";
      return false;
    }
  }
  return true;
}

} // namespace

bool parseAppManifest(const std::string &json, AppManifest &manifest,
                      std::string &error) {
  JsonValue root;
  if (!parseJson(json, root, error)) {
    error = "parse manifest.json: " + error;
    return false;
  }
  if (!root.isObject()) {
    error = "manifest.json must contain an object";
    return false;
  }
  AppManifest parsed;
  const JsonValue *schema = root.get("schema");
  const JsonValue *entry = root.get("entry");
  if (!schema || !schema->isNumber() || schema->integerValue() != 1) {
    error = "manifest field 'schema' must be integer 1";
    return false;
  }
  parsed.schema = 1;
  if (!entry) {
    error = "manifest field 'entry' is required";
    return false;
  }
  if (!rejectLegacyFields(root, error) ||
      !onlyFields(root,
                  {"schema", "id", "name", "version", "role", "entry",
                   "modules", "permissions", "handlers"},
                  "manifest", error) ||
      !requiredString(root, "id", parsed.id, error) ||
      !requiredString(root, "name", parsed.name, error) ||
      !requiredString(root, "version", parsed.version, error) ||
      !optionalString(root, "role", parsed.role, error) ||
      !collectPermissions(root.get("permissions"), parsed, error) ||
      !collectHandlers(root.get("handlers"), parsed, error) ||
      !parseExecutable(*entry, kAppSourcePrefix, parsed.entry, error) ||
      !collectModules(root.get("modules"), parsed, error))
    return false;

  if (!validIdentifier(parsed.id) || !validVersion(parsed.version)) {
    error = "manifest id must be a lowercase reverse-domain identifier and "
            "version must contain only supported characters";
    return false;
  }
  manifest = std::move(parsed);
  error.clear();
  return true;
}

const char *appRuntimeKindName(AppRuntimeKind runtime) {
  return runtime == AppRuntimeKind::JavaScript ? "js" : "wasm";
}

} // namespace oos::apps
