#pragma once

#include <string>
#include <vector>

namespace oos::apps {

inline constexpr char kAppManifestPath[] = "manifest.json";
inline constexpr char kWasmModulePath[] = "entry.wasm";
inline constexpr char kAotModulePath[] = "entry.aot";

struct AppHandler {
  std::string kind;
  std::string value;
};

struct AppManifest {
  std::string id;
  std::string name;
  std::string version;
  std::string role;
  std::vector<std::string> requested_permissions;
  std::vector<AppHandler> handlers;
};

bool parseAppManifest(const std::string &json, AppManifest &manifest,
                      std::string &error);
} // namespace oos::apps
