#include "oos/apps/app_manifest.h"

#include "oos/apps/json.h"

#include <cctype>

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
  if (value.empty() || value.size() > 128)
    return false;

  size_t separators = 0;
  bool segment_start = true;
  for (const unsigned char character : value) {
    if (character == '.') {
      if (segment_start)
        return false;
      ++separators;
      segment_start = true;
      continue;
    }
    if (segment_start) {
      if (character < 'a' || character > 'z')
        return false;
      segment_start = false;
      continue;
    }
    if ((character < 'a' || character > 'z') &&
        (character < '0' || character > '9'))
      return false;
  }
  return !segment_start && separators >= 2;
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

bool rejectLegacyFields(const JsonValue &root, std::string &error) {
  constexpr const char *legacy_fields[] = {
      "format",     "package_kind",        "runtime_kind", "api_profile",
      "entrypoint", "fallback_entrypoint", "memory",
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
  if (!rejectLegacyFields(root, error) ||
      !requiredString(root, "id", parsed.id, error) ||
      !requiredString(root, "name", parsed.name, error) ||
      !requiredString(root, "version", parsed.version, error) ||
      !optionalString(root, "role", parsed.role, error) ||
      !collectPermissions(root.get("permissions"), parsed, error))
    return false;

  if (!validIdentifier(parsed.id) || !validVersion(parsed.version)) {
    error = "manifest id must be a lowercase reverse-domain identifier and "
            "version must contain only supported characters";
    return false;
  }
  manifest = std::move(parsed);
  return true;
}

} // namespace oos::apps
