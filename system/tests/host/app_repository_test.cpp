#include "oos/apps/app_manifest.h"
#include "oos/apps/app_repository.h"
#include "oos/apps/permissions.h"
#include "oos/storage/app_storage.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

bool check(bool condition, const char *message) {
  if (!condition)
    std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

bool hasPermission(const oos::apps::AppRecord &record,
                   const char *permission) {
  for (const std::string &requested : record.manifest.requested_permissions) {
    if (requested == permission)
      return true;
  }
  return false;
}

bool hasHandler(const oos::apps::AppRecord &record, const char *kind,
                const char *value) {
  for (const oos::apps::AppHandler &handler : record.manifest.handlers) {
    if (handler.kind == kind && handler.value == value)
      return true;
  }
  return false;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2 && argc != 4 && argc != 5) {
    std::fprintf(stderr,
                 "usage: %s OOS.zip [KAIOS25.zip KAIOS3.zip "
                 "REAL_KAIOS3.zip]\n",
                 argv[0]);
    return 2;
  }
  char root_template[] = "/tmp/oos-app-test.XXXXXX";
  const char *root = mkdtemp(root_template);
  if (!root) {
    std::perror("mkdtemp");
    return 1;
  }

  oos::apps::AppManifest invalid;
  std::string error;
  bool success = check(
      !oos::apps::parseAppManifest(
          R"({"format":1,"id":"../bad","name":"bad","version":"1.0.0","package_kind":"oos-wasm-v1","runtime_kind":"wamr","api_profile":"oos-wit-0.1","entrypoint":"../app.aot"})",
          invalid, error),
      "unsafe manifest must be rejected");
  success &= check(
      !oos::apps::parseAppManifest(
          R"({"format":1,"id":"org.orangeos.bad","name":"bad","version":"1.0.0","package_kind":"oos-wasm-v1","runtime_kind":"wamr","api_profile":"oos-wit-0.1","entrypoint":"app.aot","permissions":["camera"]})",
          invalid, error),
      "malformed permissions must be rejected");

  oos::apps::AppRepository repository(root);
  success &= check(repository.initialize(), repository.lastError().c_str());
  oos::apps::AppRecord installed;
  success &= check(repository.install(argv[1], &installed),
                   repository.lastError().c_str());
  success &=
      check(installed.manifest.id == "org.orangeos.test", "installed app id");
  success &=
      check(installed.manifest.runtime_kind == oos::apps::RuntimeKind::Wamr,
            "installed runtime discriminator");
  success &= check(hasPermission(installed, "camera") &&
                       hasPermission(installed, "wifi-manage"),
                   "OOS manifest permissions survive registry resolution");

  if (argc >= 4) {
    oos::apps::AppInstallOptions kaios25;
    kaios25.app_id = "org.kaios.apnconfig";
    oos::apps::AppRecord webapp;
    success &= check(repository.install(argv[2], kaios25, &webapp),
                     repository.lastError().c_str());
    success &=
        check(webapp.manifest.package_kind == oos::apps::PackageKind::KaiOs2 &&
                  webapp.manifest.api_profile == "kaios-v2",
              "KaiOS 2.5 runtime discriminator");
    success &= check(hasPermission(webapp, "device-storage:sdcard") &&
                         hasPermission(webapp, "wifi-manage") &&
                         hasPermission(webapp, "settings:read") &&
                         hasPermission(webapp, "settings:write") &&
                         hasPermission(webapp, "system-message:alarm") &&
                         hasPermission(
                             webapp,
                             "datastore-owned:readwrite:test-state") &&
                         hasPermission(
                             webapp,
                             "datastore-owned:readonly:owner-write"),
                     "KaiOS 2.5 permissions survive registry resolution");
    success &= check(hasHandler(webapp, "activity", "pick"),
                     "KaiOS 2.5 activity handler survives registry resolution");
    const auto owner_stores = oos::apps::ownedDataStoreGrants(
        webapp.manifest.requested_permissions);
    success &= check(owner_stores.size() == 2 && owner_stores[0].writable &&
                         owner_stores[1].writable,
                     "DataStore owners always retain write access");
    oos::apps::AppInstallOptions kaios3;
    kaios3.app_id = "org.kaios.calculator";
    oos::apps::AppRecord webmanifest;
    success &= check(repository.install(argv[3], kaios3, &webmanifest),
                     repository.lastError().c_str());
    success &= check(webmanifest.manifest.package_kind ==
                             oos::apps::PackageKind::KaiOs3 &&
                         webmanifest.manifest.api_profile == "kaios-v3",
                     "KaiOS 3 runtime discriminator");
    success &= check(hasPermission(webmanifest, "bluetooth") &&
                         hasPermission(webmanifest, "camera") &&
                         hasPermission(webmanifest, "settings:read") &&
                         hasPermission(webmanifest, "settings:write") &&
                         hasPermission(webmanifest, "system-message:alarm"),
                     "KaiOS 3 permissions survive registry resolution");
    success &= check(hasHandler(webmanifest, "activity", "view"),
                     "KaiOS 3 activity handler survives registry resolution");
    oos::apps::AppLaunch web_launch;
    success &=
        check(repository.prepareLaunch("org.kaios.calculator", web_launch),
              repository.lastError().c_str());
    success &= check(web_launch.executable_path == webmanifest.package_path,
                     "WPE launch keeps the ZIP as canonical source");
    success &= check(web_launch.entrypoint == webmanifest.manifest.entrypoint,
                     "WPE launch exposes the resolved ZIP entrypoint");
    success &= check(web_launch.entrypoint == "main.html",
                     "WPE launch strips query parameters from start_url");
    success &= check(hasPermission(web_launch.app, "bluetooth") &&
                         hasPermission(web_launch.app, "camera"),
                     "WPE launch receives granted permissions");
    if (argc == 5) {
      oos::apps::AppInstallOptions real_kaios3;
      real_kaios3.app_id = "omnij2me";
      oos::apps::AppRecord real_webmanifest;
      success &=
          check(repository.install(argv[4], real_kaios3, &real_webmanifest),
                repository.lastError().c_str());
      oos::apps::AppLaunch real_web_launch;
      success &= check(repository.prepareLaunch("omnij2me", real_web_launch),
                       repository.lastError().c_str());
      success &= check(real_webmanifest.manifest.package_kind ==
                               oos::apps::PackageKind::KaiOs3 &&
                           real_web_launch.entrypoint == "index.html",
                       "real KaiOS 3 relative start_url is normalized");
    }
  }

  std::vector<oos::apps::AppRecord> records;
  success &= check(repository.list(records), repository.lastError().c_str());
  const size_t expected_records = argc == 5 ? 4u : (argc == 4 ? 3u : 1u);
  success &= check(records.size() == expected_records, "registry app count");

  oos::apps::AppLaunch launch;
  success &= check(repository.prepareLaunch("org.orangeos.test", launch),
                   repository.lastError().c_str());
  success &= check(access(launch.executable_path.c_str(), R_OK) == 0,
                   "AOT cache file is materialized");
  success &= check(!launch.entrypoint.empty(), "native entrypoint is resolved");
  success &= check(launch.data_directory.find(
                       "users/0/wasm/org.orangeos.test") != std::string::npos,
                   "per-app data directory");

  oos::storage::AppStorage storage(launch.data_directory);
  success &= check(storage.initialize(), storage.lastError().c_str());
  const uint8_t expected[] = {0, 1, 2, 3, 255};
  success &= check(storage.set("state", expected, sizeof(expected)),
                   storage.lastError().c_str());
  std::vector<uint8_t> actual;
  bool found = false;
  success &=
      check(storage.get("state", actual, found), storage.lastError().c_str());
  success &=
      check(found && actual == std::vector<uint8_t>(expected, expected + 5),
            "KV round trip");
  bool removed = false;
  success &= check(storage.remove("state", removed) && removed,
                   storage.lastError().c_str());

  std::unique_ptr<oos::storage::SqliteDatabase> database;
  success &= check(storage.openDatabase("contacts", database),
                   storage.lastError().c_str());
  success &=
      check(database &&
                database->exec("CREATE TABLE sample(id INTEGER PRIMARY KEY);"),
            database ? database->lastError().c_str() : "database open");

  std::filesystem::remove_all(root);
  if (success)
    std::fprintf(stderr, "PASS: ZIP registry and app storage\n");
  return success ? 0 : 1;
}
