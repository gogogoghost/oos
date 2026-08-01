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

bool hasColumn(oos::storage::SqliteDatabase &database, const char *table,
               const char *column, bool &found, std::string &error) {
  oos::storage::SqliteStatement statement;
  const std::string sql = std::string("PRAGMA table_info(") + table + ");";
  if (!database.prepare(sql.c_str(), statement)) {
    error = database.lastError();
    return false;
  }
  found = false;
  while (true) {
    const auto result = statement.step();
    if (result == oos::storage::SqliteStatement::Step::Done)
      return true;
    const char *name = statement.columnText(1);
    if (result != oos::storage::SqliteStatement::Step::Row || !name) {
      error = database.lastError();
      return false;
    }
    if (std::strcmp(name, column) == 0)
      found = true;
  }
}

bool registrySchemaIsSimplified(const std::string &root, std::string &error) {
  oos::storage::SqliteDatabase database;
  if (!database.open(root + "/system/app-registry.sqlite3")) {
    error = database.lastError();
    return false;
  }
  oos::storage::SqliteStatement version;
  if (!database.prepare("PRAGMA user_version;", version) ||
      version.step() != oos::storage::SqliteStatement::Step::Row ||
      version.columnInt64(0) != 3) {
    error = "application registry was not migrated to schema 3";
    return false;
  }
  struct ObsoleteColumn {
    const char *table;
    const char *column;
  };
  constexpr ObsoleteColumn obsolete[] = {
      {"applications", "package_kind"},        {"applications", "runtime_kind"},
      {"applications", "api_profile"},         {"app_versions", "entrypoint"},
      {"app_versions", "fallback_entrypoint"}, {"app_versions", "stack_bytes"},
      {"app_versions", "heap_bytes"},
  };
  for (const auto &item : obsolete) {
    bool found = false;
    if (!hasColumn(database, item.table, item.column, found, error))
      return false;
    if (found) {
      error = std::string("obsolete registry column remains: ") + item.table +
              "." + item.column;
      return false;
    }
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s OOS.zip entry.aot|entry.wasm\n", argv[0]);
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
          R"({"id":"../bad","name":"bad","version":"1.0.0"})", invalid, error),
      "unsafe manifest must be rejected");
  success &= check(
      !oos::apps::parseAppManifest(
          R"({"id":"cc.jaxy.oos.bad","name":"bad","version":"1.0.0","permissions":["camera"]})",
          invalid, error),
      "malformed permissions must be rejected");
  success &= check(
      !oos::apps::parseAppManifest(
          R"({"id":"cc.jaxy.oos.old","name":"old","version":"1.0.0","runtime_kind":"wamr"})",
          invalid, error),
      "obsolete runtime fields must be rejected");

  success &= check(seedLegacyWebRecord(root, error), error.c_str());
  oos::apps::AppRepository repository(root);
  const bool initialized = repository.initialize();
  success &= check(initialized, repository.lastError().c_str());
  if (!initialized) {
    std::filesystem::remove_all(root);
    return 1;
  }
  success &= check(legacyRowsRemoved(root, error),
                   "schema-1 Web records must be removed from every table");
  success &= check(registrySchemaIsSimplified(root, error), error.c_str());
  oos::apps::AppRecord installed;
  const bool package_installed = repository.install(argv[1], &installed);
  success &= check(package_installed, repository.lastError().c_str());
  success &=
      check(installed.manifest.id == "cc.jaxy.oos.test", "installed app id");
  success &= check(hasPermission(installed, "camera") &&
                       hasPermission(installed, "wifi-manage"),
                   "OOS manifest permissions survive registry resolution");

  std::vector<oos::apps::AppRecord> records;
  const bool listed = repository.list(records);
  success &= check(listed, repository.lastError().c_str());
  success &= check(records.size() == 1, "registry app count");

  oos::apps::AppLaunch launch;
  const bool launch_prepared =
      repository.prepareLaunch("cc.jaxy.oos.test", launch);
  success &= check(launch_prepared, repository.lastError().c_str());
  success &= check(access(launch.executable_path.c_str(), R_OK) == 0,
                   "AOT cache file is materialized");
  success &= check(!launch.entrypoint.empty(), "native entrypoint is resolved");
  success &= check(launch.entrypoint == argv[2],
                   "package entry is accepted and selected");
  success &= check(launch.data_directory.find(
                       "users/0/wasm/cc.jaxy.oos.test") != std::string::npos,
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
