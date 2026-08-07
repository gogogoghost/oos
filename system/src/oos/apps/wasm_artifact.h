#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace oos::apps {

struct WasmTargetProfile {
  std::string cpu_core;
  std::string cpu_arch;
};

WasmTargetProfile defaultWasmTargetProfile();
bool validWasmTargetName(std::string_view name);
bool isSuffixlessWasmBasePath(std::string_view path);
std::vector<std::string>
wasmArtifactCandidates(const std::string &base_path,
                       const WasmTargetProfile &target);
bool resolveWasmArtifact(const std::string &base_path,
                         const WasmTargetProfile &target,
                         std::string &artifact_path, std::string &error);
bool isWasmArtifactForBase(std::string_view artifact_path,
                           std::string_view base_path);

} // namespace oos::apps
