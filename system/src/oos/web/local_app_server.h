#pragma once

#include "oos/apps/zip_archive.h"

#include <atomic>
#include <pthread.h>
#include <string>

namespace oos::web {

// Serves one installed package on the KaiOS 3 signed-app origin convention.
// Only loopback clients and the selected <app-id>.localhost Host are accepted.
class LocalAppServer {
public:
  LocalAppServer(std::string app_id, std::string package_path,
                 std::string entrypoint);
  ~LocalAppServer();

  LocalAppServer(const LocalAppServer &) = delete;
  LocalAppServer &operator=(const LocalAppServer &) = delete;

  bool start();
  void stop();

  std::string urlFor(const std::string &path) const;
  const std::string &origin() const { return origin_; }
  const std::string &lastError() const { return error_; }

private:
  static void *serveThread(void *context);
  void serve();
  void handleClient(int client);

  std::string app_id_;
  std::string package_path_;
  std::string entrypoint_;
  std::string host_;
  std::string origin_;
  apps::ZipArchive archive_;
  int listener_ = -1;
  std::atomic<bool> stopping_{false};
  pthread_t thread_ = {};
  bool thread_started_ = false;
  std::string error_;
};

} // namespace oos::web
