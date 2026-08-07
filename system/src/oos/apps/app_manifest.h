#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oos::apps {

inline constexpr char kAppManifestPath[] = "manifest.json";
inline constexpr char kAppSourcePrefix[] = "app/";
inline constexpr char kModulePrefix[] = "modules/";

enum class AppRuntimeKind { JavaScript, WebAssembly };

struct AppEntrypoint {
  AppRuntimeKind runtime = AppRuntimeKind::WebAssembly;
  std::string path;
};

struct AppModule {
  std::string name;
  AppRuntimeKind runtime = AppRuntimeKind::WebAssembly;
  std::string path;
};

struct AppHandler {
  std::string kind;
  std::string value;
};

struct AppManifest {
  uint32_t schema = 0;
  std::string id;
  std::string name;
  std::string version;
  std::string role;
  std::vector<std::string> requested_permissions;
  std::vector<AppHandler> handlers;
  AppEntrypoint entry;
  std::vector<AppModule> modules;
};

const char *appRuntimeKindName(AppRuntimeKind runtime);

bool parseAppManifest(const std::string &json, AppManifest &manifest,
                      std::string &error);
} // namespace oos::apps
