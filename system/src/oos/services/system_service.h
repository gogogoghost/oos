#pragma once

#include <memory>
#include <string>
#include <vector>

namespace oos::apps {
class AppRepository;
}

namespace oos::services {

// Owns persistent policy state used by KaiOS compatibility adapters and the
// future SystemUI. Device drivers are intentionally outside this service.
class SystemServiceHub {
public:
  SystemServiceHub(std::string data_root,
                   apps::AppRepository *applications = nullptr);
  ~SystemServiceHub();

  SystemServiceHub(const SystemServiceHub &) = delete;
  SystemServiceHub &operator=(const SystemServiceHub &) = delete;

  bool initialize();

  // Payload and response are JSON because the WPE boundary already speaks
  // JSON. The WIT adapter exposes the same operation without another model.
  int request(const std::string &app_id,
              const std::vector<std::string> &permissions,
              const std::string &service, const std::string &operation,
              const std::string &payload, std::string &response,
              bool system_authority = false);

  const std::string &lastError() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::services
