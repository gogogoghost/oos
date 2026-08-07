#include "oos/apps/app_manifest.h"
#include "oos/apps/app_repository.h"
#include "oos/apps/wasm_artifact.h"
#include "oos/resources/package_assets.h"
#include "oos/storage/app_storage.h"
#include "oos/storage/sqlite.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

bool artifactPriorityIsCorrect(const std::string &root) {
  const std::string directory = root + "/artifact-priority";
  const std::string base = directory + "/main";
  std::filesystem::create_directories(directory);
  const std::string core = base + ".cortex-a53.aot";
  const std::string arch = base + ".armv7a.aot";
  const std::string wasm = base + ".wasm";
  std::ofstream(core).put('c');
  std::ofstream(arch).put('a');
  std::ofstream(wasm).put('w');

  std::string resolved;
  std::string error;
  const oos::apps::WasmTargetProfile target{"cortex-a53", "armv7a"};
  bool success =
      check(oos::apps::resolveWasmArtifact(base, target, resolved, error) &&
                resolved == core,
            "CPU core AOT must have highest priority");
  std::filesystem::remove(core);
  success &=
      check(oos::apps::resolveWasmArtifact(base, target, resolved, error) &&
                resolved == arch,
            "CPU architecture AOT must be the second choice");
  std::filesystem::remove(arch);
  success &=
      check(oos::apps::resolveWasmArtifact(base, target, resolved, error) &&
                resolved == wasm,
            "portable Wasm must be the final fallback");
  success &= check(
      oos::apps::isWasmArtifactForBase(core, base) &&
          oos::apps::isWasmArtifactForBase(arch, base) &&
          oos::apps::isWasmArtifactForBase(wasm, base) &&
          !oos::apps::isWasmArtifactForBase(base + ".aot", base),
      "only target-qualified AOT and portable Wasm artifacts are accepted");
  success &= check(
      !oos::apps::resolveWasmArtifact(base + ".wasm", target, resolved, error),
      "runtime Wasm paths with a suffix must be rejected");
  return success;
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
    std::fprintf(stderr, "usage: %s OOS.zip app/main.ARCH.aot|app/main.wasm\n",
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
  success &= artifactPriorityIsCorrect(root);
  oos::apps::AppManifest js_manifest;
  success &= check(
      oos::apps::parseAppManifest(
          R"({"schema":1,"id":"cc.jaxy.oos.js","name":"JS","version":"1.0.0","entry":{"runtime":"js","path":"app/main.mjs"},"modules":[{"name":"worker","runtime":"wasm","path":"modules/worker"}]})",
          js_manifest, error),
      error.c_str());
  success &= check(js_manifest.entry.runtime ==
                           oos::apps::AppRuntimeKind::JavaScript &&
                       js_manifest.entry.path == "app/main.mjs" &&
                       js_manifest.modules.size() == 1,
                   "JavaScript entry and cross-runtime modules are parsed");
  success &= check(
      !oos::apps::parseAppManifest(
          R"({"schema":1,"id":"cc.jaxy.oos.escape","name":"bad","version":"1.0.0","entry":{"runtime":"js","path":"app/../main.js"}})",
          invalid, error),
      "entry path traversal must be rejected");
  success &= check(
      !oos::apps::parseAppManifest(
          R"({"schema":1,"id":"cc.jaxy.oos.unknown","name":"bad","version":"1.0.0","entry":{"runtime":"js","path":"app/main.js"},"ui":"raw"})",
          invalid, error),
      "unknown or obsolete application-level UI fields must be rejected");
  success &= check(
      !oos::apps::parseAppManifest(
          R"({"schema":1,"id":"cc.jaxy.oos.unknown","name":"bad","version":"1.0.0","entry":{"runtime":"js","path":"app/main.js","extra":true}})",
          invalid, error),
      "unknown entry fields must be rejected");
  success &= check(
      !oos::apps::parseAppManifest(
          R"({"schema":1,"id":"cc.jaxy.oos.permission","name":"bad","version":"1.0.0","entry":{"runtime":"js","path":"app/main.js"},"permissions":{"settings":{"access":"all"}}})",
          invalid, error),
      "unknown permission access modes must be rejected");
  success &= check(
      !oos::apps::parseAppManifest(
          R"({"schema":1,"id":"cc.jaxy.oos.suffix","name":"bad","version":"1.0.0","entry":{"runtime":"wasm","path":"app/main.wasm"}})",
          invalid, error),
      "Wasm manifest paths with file suffixes must be rejected");

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
  success &= check(installed.manifest.entry.runtime ==
                           oos::apps::AppRuntimeKind::WebAssembly &&
                       installed.manifest.handlers.size() == 1,
                   "entry and handlers survive registry resolution");

  std::vector<oos::apps::AppRecord> records;
  const bool listed = repository.list(records);
  success &= check(listed, repository.lastError().c_str());
  success &= check(records.size() == 1, "registry app count");

  oos::apps::AppLaunch launch;
  const bool launch_prepared =
      repository.prepareLaunch("cc.jaxy.oos.test", launch);
  success &= check(launch_prepared, repository.lastError().c_str());
  success &=
      check(access((launch.cache_directory + "/" + argv[2]).c_str(), R_OK) == 0,
            "selected package artifact is materialized");
  success &= check(!launch.entrypoint.empty(), "native entrypoint is resolved");
  success &=
      check(launch.entrypoint == "app/main" &&
                launch.executable_path == launch.cache_directory + "/app/main",
            "repository exposes a suffixless Wasm base path");
  success &= check(
      access((launch.module_directory + "/compiler.wasm").c_str(), R_OK) == 0 &&
          access((launch.module_directory + "/runtime.cortex-a53.aot").c_str(),
                 R_OK) == 0,
      "packaged modules are validated and materialized");
  oos::resources::PackageAssetService assets(launch.asset_directory);
  uint32_t asset_handle = 0;
  uint64_t asset_size = 0;
  success &= check(assets.open("audio/test.dat", asset_handle, asset_size) &&
                       asset_size == 19,
                   assets.lastError().c_str());
  std::vector<uint8_t> asset_bytes;
  success &= check(assets.read(asset_handle, 4, 8, asset_bytes) &&
                       std::string(asset_bytes.begin(), asset_bytes.end()) ==
                           "packaged",
                   assets.lastError().c_str());
  success &= check(assets.close(asset_handle), assets.lastError().c_str());
  success &= check(!assets.open("../entry.aot", asset_handle, asset_size),
                   "asset traversal must be rejected");
  success &= check(launch.data_directory.find(
                       "users/0/apps/cc.jaxy.oos.test") != std::string::npos,
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

  database.reset();
  success &= check(repository.uninstall("cc.jaxy.oos.test"),
                   repository.lastError().c_str());
  records.clear();
  success &= check(repository.list(records) && records.empty(),
                   "uninstalled application is absent from registry");
  success &= check(access(launch.data_directory.c_str(), F_OK) != 0,
                   "uninstall removes application data");
  success &= check(access(launch.cache_directory.c_str(), F_OK) != 0,
                   "uninstall removes extracted executable cache");

  std::filesystem::remove_all(root);
  if (success)
    std::fprintf(stderr, "PASS: ZIP registry and app storage\n");
  return success ? 0 : 1;
}
