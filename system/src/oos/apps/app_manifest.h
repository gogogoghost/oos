#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oos::apps {

enum class PackageKind { OosWasmV1 };
enum class RuntimeKind { Wamr };

struct AppHandler {
  std::string kind;
  std::string value;
};

struct AppManifest {
  uint32_t format = 0;
  std::string id;
  std::string name;
  std::string version;
  PackageKind package_kind = PackageKind::OosWasmV1;
  RuntimeKind runtime_kind = RuntimeKind::Wamr;
  std::string api_profile;
  std::string entrypoint;
  std::string fallback_entrypoint;
  std::string role;
  uint32_t stack_bytes = 128 * 1024;
  uint32_t heap_bytes = 4 * 1024 * 1024;
  std::vector<std::string> requested_permissions;
  std::vector<AppHandler> handlers;
};

const char *packageKindName(PackageKind kind);
const char *runtimeKindName(RuntimeKind kind);
bool parseAppManifest(const std::string &json, AppManifest &manifest,
                      std::string &error);
} // namespace oos::apps
