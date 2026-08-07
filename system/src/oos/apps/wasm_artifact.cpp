#include "oos/apps/wasm_artifact.h"

#include <sys/stat.h>

#include <algorithm>

namespace oos::apps {
namespace {

std::string compiledArchitecture() {
#if defined(OOS_WASM_CPU_ARCH)
  return OOS_WASM_CPU_ARCH;
#elif defined(__x86_64__)
  return "x86_64";
#elif defined(__i386__)
  return "x86";
#elif defined(__aarch64__)
  return "aarch64";
#elif defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7
  return "armv7a";
#elif defined(__arm__)
  return "arm";
#else
  return {};
#endif
}

bool regularFile(const std::string &path) {
  struct stat status = {};
  return stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

} // namespace

WasmTargetProfile defaultWasmTargetProfile() {
  WasmTargetProfile target;
#if defined(OOS_WASM_CPU_CORE)
  target.cpu_core = OOS_WASM_CPU_CORE;
#endif
  target.cpu_arch = compiledArchitecture();
  return target;
}

bool validWasmTargetName(std::string_view name) {
  if (name.empty() || name.size() > 64 || name.front() == '-' ||
      name.front() == '_' || name.back() == '-' || name.back() == '_')
    return false;
  return std::all_of(name.begin(), name.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '-' ||
           character == '_';
  });
}

bool isSuffixlessWasmBasePath(std::string_view path) {
  if (path.empty() || path.back() == '/')
    return false;
  const size_t separator = path.find_last_of('/');
  const size_t name_start =
      separator == std::string_view::npos ? 0 : separator + 1;
  const std::string_view name = path.substr(name_start);
  return !name.empty() && name != "." && name != ".." &&
         name.find('.') == std::string_view::npos;
}

std::vector<std::string>
wasmArtifactCandidates(const std::string &base_path,
                       const WasmTargetProfile &target) {
  std::vector<std::string> candidates;
  const auto appendAot = [&](const std::string &name) {
    if (!validWasmTargetName(name))
      return;
    const std::string candidate = base_path + "." + name + ".aot";
    if (std::find(candidates.begin(), candidates.end(), candidate) ==
        candidates.end())
      candidates.push_back(candidate);
  };
  appendAot(target.cpu_core);
  appendAot(target.cpu_arch);
  candidates.push_back(base_path + ".wasm");
  return candidates;
}

bool resolveWasmArtifact(const std::string &base_path,
                         const WasmTargetProfile &target,
                         std::string &artifact_path, std::string &error) {
  artifact_path.clear();
  if (!isSuffixlessWasmBasePath(base_path)) {
    error =
        "Wasm module base path must not include a file suffix: " + base_path;
    return false;
  }
  const std::vector<std::string> candidates =
      wasmArtifactCandidates(base_path, target);
  for (const std::string &candidate : candidates) {
    if (regularFile(candidate)) {
      artifact_path = candidate;
      error.clear();
      return true;
    }
  }
  error =
      "no compatible Wasm artifact for base path '" + base_path + "' (tried";
  for (const std::string &candidate : candidates)
    error += " " + candidate;
  error += ")";
  return false;
}

bool isWasmArtifactForBase(std::string_view artifact_path,
                           std::string_view base_path) {
  if (artifact_path == std::string(base_path) + ".wasm")
    return true;
  if (artifact_path.size() <= base_path.size() + 5 ||
      artifact_path.compare(0, base_path.size(), base_path) != 0 ||
      artifact_path[base_path.size()] != '.' ||
      artifact_path.substr(artifact_path.size() - 4) != ".aot")
    return false;
  const std::string_view target = artifact_path.substr(
      base_path.size() + 1, artifact_path.size() - base_path.size() - 5);
  return validWasmTargetName(target);
}

} // namespace oos::apps
