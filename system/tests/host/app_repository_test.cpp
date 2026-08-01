#include "oos/apps/app_manifest.h"
#include "oos/apps/app_repository.h"
#include "oos/storage/app_storage.h"
#include "oos/storage/sqlite.h"

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

bool hasPermission(const oos::apps::AppRecord &record, const char *permission) {
  for (const std::string &requested : record.manifest.requested_permissions) {
    if (requested == permission)
      return true;
  }
  return false;
}

bool seedLegacyWebRecord(const std::string &root, std::string &error) {
  std::filesystem::create_directories(root + "/system");
  oos::storage::SqliteDatabase database;
  if (!database.open(root + "/system/app-registry.sqlite3") ||
      !database.exec(
          "CREATE TABLE applications("
          " app_id TEXT PRIMARY KEY, display_name TEXT NOT NULL,"
          " active_version TEXT NOT NULL, package_kind TEXT NOT NULL,"
          " runtime_kind TEXT NOT NULL, api_profile TEXT NOT NULL,"
          " role TEXT NOT NULL DEFAULT '', enabled INTEGER NOT NULL DEFAULT 1,"
          " trust_level TEXT NOT NULL DEFAULT 'unverified',"
          " installed_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
          "CREATE TABLE app_versions("
          " app_id TEXT NOT NULL, version TEXT NOT NULL,"
          " package_path TEXT NOT NULL, package_digest TEXT NOT NULL,"
          " entrypoint TEXT NOT NULL, fallback_entrypoint TEXT NOT NULL,"
          " stack_bytes INTEGER NOT NULL, heap_bytes INTEGER NOT NULL,"
          " installed_at INTEGER NOT NULL, PRIMARY KEY(app_id, version));"
          "CREATE TABLE app_permissions("
          " app_id TEXT NOT NULL, permission TEXT NOT NULL,"
          " requested INTEGER NOT NULL, granted INTEGER NOT NULL,"
          " PRIMARY KEY(app_id, permission));"
          "CREATE TABLE app_roles(app_id TEXT NOT NULL, role TEXT NOT NULL,"
          " PRIMARY KEY(app_id, role));"
          "CREATE TABLE app_handlers("
          " app_id TEXT NOT NULL, handler_kind TEXT NOT NULL,"
          " handler_value TEXT NOT NULL,"
          " PRIMARY KEY(app_id, handler_kind, handler_value));"
          "CREATE TABLE app_state("
          " app_id TEXT PRIMARY KEY, state TEXT NOT NULL,"
          " last_started_at INTEGER, last_exit_code INTEGER);"
          "INSERT INTO applications VALUES("
          " 'org.orangeos.legacy-web','Legacy Web','1.0.0','kaios-v3','wpe',"
          " 'kaios-v3','',1,'unverified',1,1);"
          "INSERT INTO app_versions VALUES("
          " 'org.orangeos.legacy-web','1.0.0','/legacy.zip','digest',"
          " 'index.html','',131072,4194304,1);"
          "INSERT INTO app_permissions VALUES("
          " 'org.orangeos.legacy-web','camera',1,1);"
          "INSERT INTO app_roles VALUES('org.orangeos.legacy-web','launcher');"
          "INSERT INTO app_handlers VALUES("
          " 'org.orangeos.legacy-web','activity','view');"
          "INSERT INTO app_state VALUES("
          " 'org.orangeos.legacy-web','installed',NULL,NULL);"
          "PRAGMA user_version=1;")) {
    error = database.lastError();
    return false;
  }
  return true;
}

bool legacyRowsRemoved(const std::string &root, std::string &error) {
  oos::storage::SqliteDatabase database;
  if (!database.open(root + "/system/app-registry.sqlite3")) {
    error = database.lastError();
    return false;
  }
  constexpr const char *tables[] = {"applications",    "app_versions",
                                    "app_permissions", "app_roles",
                                    "app_handlers",    "app_state"};
  for (const char *table : tables) {
    oos::storage::SqliteStatement statement;
    const std::string sql = std::string("SELECT COUNT(*) FROM ") + table +
                            " WHERE app_id='org.orangeos.legacy-web';";
    if (!database.prepare(sql.c_str(), statement) ||
        statement.step() != oos::storage::SqliteStatement::Step::Row ||
        statement.columnInt64(0) != 0) {
      error = database.lastError();
      return false;
    }
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s OOS.zip\n", argv[0]);
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
  success &= check(
      !oos::apps::parseAppManifest(
          R"({"format":1,"id":"org.orangeos.web","name":"web","version":"1.0.0","package_kind":"oos-wasm-v1","runtime_kind":"wpe","api_profile":"oos-wit-0.1","entrypoint":"index.html"})",
          invalid, error),
      "non-WAMR runtime must be rejected");

  success &= check(seedLegacyWebRecord(root, error), error.c_str());
  oos::apps::AppRepository repository(root);
  success &= check(repository.initialize(), repository.lastError().c_str());
  success &= check(legacyRowsRemoved(root, error),
                   "schema-1 Web records must be removed from every table");
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

  std::vector<oos::apps::AppRecord> records;
  success &= check(repository.list(records), repository.lastError().c_str());
  success &= check(records.size() == 1, "registry app count");

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
