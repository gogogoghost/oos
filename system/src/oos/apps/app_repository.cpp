#include "oos/apps/app_repository.h"

#include "oos/apps/zip_archive.h"
#include "oos/storage/filesystem.h"
#include "oos/storage/sqlite.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace oos::apps {
namespace {

constexpr size_t kMaximumManifestBytes = 256 * 1024;
constexpr size_t kMaximumAssetBytes = 64 * 1024 * 1024;
constexpr const char kAssetPrefix[] = "assets/";

std::string join(const std::string &left, const std::string &right) {
  return left + (left.empty() || left.back() == '/' ? "" : "/") + right;
}

std::string contentDigest(const char *path, std::string &error) {
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    error = std::string("open package for digest: ") + std::strerror(errno);
    return {};
  }
  uint64_t hash = 1469598103934665603ULL;
  uint8_t buffer[64 * 1024];
  while (true) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      error = std::string("read package for digest: ") + std::strerror(errno);
      close(fd);
      return {};
    }
    if (count == 0)
      break;
    for (ssize_t index = 0; index < count; ++index) {
      hash ^= buffer[index];
      hash *= 1099511628211ULL;
    }
  }
  close(fd);
  char output[17] = {};
  std::snprintf(output, sizeof(output), "%016llx",
                static_cast<unsigned long long>(hash));
  return output;
}

bool parseRecord(storage::SqliteStatement &statement, AppRecord &record) {
  const char *id = statement.columnText(0);
  const char *name = statement.columnText(1);
  const char *version = statement.columnText(2);
  const char *role = statement.columnText(3);
  const char *package_path = statement.columnText(4);
  const char *digest = statement.columnText(5);
  if (!id || !name || !version || !package_path || !digest)
    return false;

  AppRecord parsed;
  parsed.manifest.id = id;
  parsed.manifest.name = name;
  parsed.manifest.version = version;
  parsed.manifest.role = role ? role : "";
  parsed.package_path = package_path;
  parsed.package_digest = digest;
  parsed.enabled = statement.columnInt64(6) != 0;
  record = std::move(parsed);
  return true;
}

bool loadGrantedPermissions(storage::SqliteDatabase &database,
                            AppRecord &record) {
  storage::SqliteStatement permissions;
  if (!database.prepare("SELECT permission FROM app_permissions"
                        " WHERE app_id=? AND requested=1 AND granted=1"
                        " ORDER BY permission;",
                        permissions) ||
      !permissions.bindText(1, record.manifest.id))
    return false;
  record.manifest.requested_permissions.clear();
  while (true) {
    const auto result = permissions.step();
    if (result == storage::SqliteStatement::Step::Done)
      return true;
    const char *permission = permissions.columnText(0);
    if (result != storage::SqliteStatement::Step::Row || !permission)
      return false;
    record.manifest.requested_permissions.emplace_back(permission);
  }
}

bool loadHandlers(storage::SqliteDatabase &database, AppRecord &record) {
  storage::SqliteStatement handlers;
  if (!database.prepare("SELECT handler_kind,handler_value FROM app_handlers"
                        " WHERE app_id=? ORDER BY handler_kind,handler_value;",
                        handlers) ||
      !handlers.bindText(1, record.manifest.id))
    return false;
  record.manifest.handlers.clear();
  while (true) {
    const auto result = handlers.step();
    if (result == storage::SqliteStatement::Step::Done)
      return true;
    const char *kind = handlers.columnText(0);
    const char *value = handlers.columnText(1);
    if (result != storage::SqliteStatement::Step::Row || !kind || !value)
      return false;
    record.manifest.handlers.push_back({kind, value});
  }
}

bool createRegistrySchema(storage::SqliteDatabase &database) {
  return database.exec(
      "CREATE TABLE IF NOT EXISTS applications("
      " app_id TEXT PRIMARY KEY, display_name TEXT NOT NULL,"
      " active_version TEXT NOT NULL, role TEXT NOT NULL DEFAULT '',"
      " enabled INTEGER NOT NULL DEFAULT 1,"
      " trust_level TEXT NOT NULL DEFAULT 'unverified',"
      " installed_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS app_versions("
      " app_id TEXT NOT NULL, version TEXT NOT NULL,"
      " package_path TEXT NOT NULL, package_digest TEXT NOT NULL,"
      " installed_at INTEGER NOT NULL, PRIMARY KEY(app_id, version));"
      "CREATE TABLE IF NOT EXISTS app_permissions("
      " app_id TEXT NOT NULL, permission TEXT NOT NULL,"
      " requested INTEGER NOT NULL DEFAULT 1,"
      " granted INTEGER NOT NULL DEFAULT 1,"
      " PRIMARY KEY(app_id, permission));"
      "CREATE TABLE IF NOT EXISTS app_roles("
      " app_id TEXT NOT NULL, role TEXT NOT NULL,"
      " PRIMARY KEY(app_id, role));"
      "CREATE TABLE IF NOT EXISTS app_handlers("
      " app_id TEXT NOT NULL, handler_kind TEXT NOT NULL,"
      " handler_value TEXT NOT NULL,"
      " PRIMARY KEY(app_id, handler_kind, handler_value));"
      "CREATE TABLE IF NOT EXISTS app_state("
      " app_id TEXT PRIMARY KEY, state TEXT NOT NULL DEFAULT 'installed',"
      " last_started_at INTEGER, last_exit_code INTEGER);"
      "PRAGMA user_version=3;");
}

bool migrateLegacyRegistry(storage::SqliteDatabase &database) {
  if (database.exec(
          "BEGIN IMMEDIATE;"
          "ALTER TABLE applications RENAME TO applications_legacy;"
          "ALTER TABLE app_versions RENAME TO app_versions_legacy;"
          "CREATE TABLE applications("
          " app_id TEXT PRIMARY KEY, display_name TEXT NOT NULL,"
          " active_version TEXT NOT NULL, role TEXT NOT NULL DEFAULT '',"
          " enabled INTEGER NOT NULL DEFAULT 1,"
          " trust_level TEXT NOT NULL DEFAULT 'unverified',"
          " installed_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
          "CREATE TABLE app_versions("
          " app_id TEXT NOT NULL, version TEXT NOT NULL,"
          " package_path TEXT NOT NULL, package_digest TEXT NOT NULL,"
          " installed_at INTEGER NOT NULL, PRIMARY KEY(app_id, version));"
          "INSERT INTO applications(app_id,display_name,active_version,role,"
          "enabled,trust_level,installed_at,updated_at)"
          " SELECT app_id,display_name,active_version,role,enabled,trust_level,"
          "installed_at,updated_at FROM applications_legacy"
          " WHERE package_kind='oos-wasm-v1' AND runtime_kind='wamr';"
          "INSERT INTO app_versions(app_id,version,package_path,package_digest,"
          "installed_at) SELECT app_id,version,package_path,package_digest,"
          "installed_at FROM app_versions_legacy WHERE app_id IN"
          " (SELECT app_id FROM applications);"
          "DELETE FROM app_permissions WHERE app_id NOT IN"
          " (SELECT app_id FROM applications);"
          "DELETE FROM app_roles WHERE app_id NOT IN"
          " (SELECT app_id FROM applications);"
          "DELETE FROM app_handlers WHERE app_id NOT IN"
          " (SELECT app_id FROM applications);"
          "DELETE FROM app_state WHERE app_id NOT IN"
          " (SELECT app_id FROM applications);"
          "DROP TABLE applications_legacy;"
          "DROP TABLE app_versions_legacy;"
          "PRAGMA user_version=3;"
          "COMMIT;"))
    return true;
  database.exec("ROLLBACK;");
  return false;
}

} // namespace

class AppRepository::Impl {
public:
  storage::SqliteDatabase database;
};

AppRepository::AppRepository(std::string data_root)
    : impl_(std::make_unique<Impl>()), data_root_(std::move(data_root)) {}

AppRepository::~AppRepository() = default;

bool AppRepository::initialize() {
  error_.clear();
  const char *directories[] = {
      "system",  "runtime", "packages",       "users/0/wasm",    "cache/aot",
      "staging", "tmp",     "media/internal", "media/removable",
  };
  for (const char *directory : directories) {
    if (!storage::ensureDirectory(join(data_root_, directory), 0700, error_))
      return false;
  }
  const std::string database_path =
      join(data_root_, "system/app-registry.sqlite3");
  if (!impl_->database.open(database_path)) {
    error_ = impl_->database.lastError();
    return false;
  }
  int64_t version = 0;
  {
    storage::SqliteStatement schema_version;
    if (!impl_->database.prepare("PRAGMA user_version;", schema_version) ||
        schema_version.step() != storage::SqliteStatement::Step::Row) {
      error_ = impl_->database.lastError();
      return false;
    }
    version = schema_version.columnInt64(0);
  }
  if (version > 3) {
    error_ = "application registry schema is newer than this OOS runtime";
    return false;
  }
  const bool schema_ready = version == 0 ? createRegistrySchema(impl_->database)
                            : version < 3
                                ? migrateLegacyRegistry(impl_->database)
                                : createRegistrySchema(impl_->database);
  if (!schema_ready) {
    error_ = impl_->database.lastError();
    return false;
  }
  return true;
}

bool AppRepository::install(const char *package_path, AppRecord *installed) {
  return install(package_path, AppInstallOptions{}, installed);
}

bool AppRepository::install(const char *package_path,
                            const AppInstallOptions &options,
                            AppRecord *installed) {
  error_.clear();
  if (!package_path || package_path[0] == '\0') {
    error_ = "application ZIP path is empty";
    return false;
  }
  ZipArchive archive;
  if (!archive.open(package_path)) {
    error_ = archive.lastError();
    return false;
  }
  AppManifest manifest;
  if (!archive.find(kAppManifestPath)) {
    error_ = "application ZIP has no manifest.json";
    return false;
  }
  std::vector<uint8_t> manifest_bytes;
  if (!archive.read(kAppManifestPath, manifest_bytes, kMaximumManifestBytes)) {
    error_ = archive.lastError();
    return false;
  }
  const std::string manifest_json(manifest_bytes.begin(), manifest_bytes.end());
  if (!parseAppManifest(manifest_json, manifest, error_))
    return false;
  if (!options.app_id.empty() && options.app_id != manifest.id) {
    error_ = "explicit application id does not match manifest.json";
    return false;
  }
  if (!archive.find(kAotModulePath) && !archive.find(kWasmModulePath)) {
    error_ = "application package has no entry.aot or entry.wasm";
    return false;
  }
  for (const ZipEntry &entry : archive.entries()) {
    if (entry.name.rfind(kAssetPrefix, 0) != 0 ||
        entry.name.size() == sizeof(kAssetPrefix) - 1)
      continue;
    std::vector<uint8_t> verified;
    if (!archive.readEntry(entry, verified, kMaximumAssetBytes)) {
      error_ = "validate packaged asset: " + archive.lastError();
      return false;
    }
  }
  const std::string digest = contentDigest(package_path, error_);
  if (digest.empty())
    return false;
  const std::string version_directory = join(
      join(join(join(data_root_, "packages"), manifest.id), manifest.version),
      digest);
  if (!storage::ensureDirectory(version_directory, 0700, error_))
    return false;
  const std::string destination = join(version_directory, "application.zip");
  if (!storage::copyFileAtomic(package_path, destination, 0600, error_))
    return false;

  if (!impl_->database.exec("BEGIN IMMEDIATE;")) {
    error_ = impl_->database.lastError();
    return false;
  }
  bool success = false;
  do {
    storage::SqliteStatement version;
    if (!impl_->database.prepare(
            "INSERT INTO app_versions(app_id,version,package_path,"
            "package_digest,installed_at) "
            "VALUES(?,?,?,?,strftime('%s','now'))"
            " ON CONFLICT(app_id,version) DO UPDATE SET"
            " package_path=excluded.package_path,"
            " package_digest=excluded.package_digest,"
            " installed_at=excluded.installed_at;",
            version) ||
        !version.bindText(1, manifest.id) ||
        !version.bindText(2, manifest.version) ||
        !version.bindText(3, destination) || !version.bindText(4, digest) ||
        version.step() != storage::SqliteStatement::Step::Done)
      break;

    storage::SqliteStatement app;
    if (!impl_->database.prepare(
            "INSERT INTO applications(app_id,display_name,active_version,"
            "role,installed_at,updated_at)"
            " VALUES(?,?,?,?,strftime('%s','now'),strftime('%s','now'))"
            " ON CONFLICT(app_id) DO UPDATE SET"
            " display_name=excluded.display_name,"
            " active_version=excluded.active_version,"
            " role=excluded.role,"
            " updated_at=excluded.updated_at;",
            app) ||
        !app.bindText(1, manifest.id) || !app.bindText(2, manifest.name) ||
        !app.bindText(3, manifest.version) || !app.bindText(4, manifest.role) ||
        app.step() != storage::SqliteStatement::Step::Done)
      break;

    storage::SqliteStatement clear_permissions;
    if (!impl_->database.prepare("DELETE FROM app_permissions WHERE app_id=?;",
                                 clear_permissions) ||
        !clear_permissions.bindText(1, manifest.id) ||
        clear_permissions.step() != storage::SqliteStatement::Step::Done)
      break;
    bool metadata_success = true;
    for (const std::string &permission : manifest.requested_permissions) {
      storage::SqliteStatement insert_permission;
      if (!impl_->database.prepare(
              "INSERT INTO app_permissions(app_id,permission,requested,granted)"
              " VALUES(?,?,1,1);",
              insert_permission) ||
          !insert_permission.bindText(1, manifest.id) ||
          !insert_permission.bindText(2, permission) ||
          insert_permission.step() != storage::SqliteStatement::Step::Done) {
        metadata_success = false;
        break;
      }
    }
    if (!metadata_success)
      break;
    storage::SqliteStatement clear_handlers;
    if (!impl_->database.prepare("DELETE FROM app_handlers WHERE app_id=?;",
                                 clear_handlers) ||
        !clear_handlers.bindText(1, manifest.id) ||
        clear_handlers.step() != storage::SqliteStatement::Step::Done)
      break;
    for (const AppHandler &handler : manifest.handlers) {
      storage::SqliteStatement insert_handler;
      if (!impl_->database.prepare(
              "INSERT INTO app_handlers(app_id,handler_kind,handler_value)"
              " VALUES(?,?,?);",
              insert_handler) ||
          !insert_handler.bindText(1, manifest.id) ||
          !insert_handler.bindText(2, handler.kind) ||
          !insert_handler.bindText(3, handler.value) ||
          insert_handler.step() != storage::SqliteStatement::Step::Done) {
        metadata_success = false;
        break;
      }
    }
    if (!metadata_success)
      break;
    storage::SqliteStatement clear_roles;
    if (!impl_->database.prepare("DELETE FROM app_roles WHERE app_id=?;",
                                 clear_roles) ||
        !clear_roles.bindText(1, manifest.id) ||
        clear_roles.step() != storage::SqliteStatement::Step::Done)
      break;
    storage::SqliteStatement state;
    if (!impl_->database.prepare(
            "INSERT INTO app_state(app_id,state) VALUES(?,'installed')"
            " ON CONFLICT(app_id) DO UPDATE SET state='installed';",
            state) ||
        !state.bindText(1, manifest.id) ||
        state.step() != storage::SqliteStatement::Step::Done)
      break;
    if (!manifest.role.empty()) {
      storage::SqliteStatement role;
      if (!impl_->database.prepare(
              "INSERT OR IGNORE INTO app_roles(app_id,role) VALUES(?,?);",
              role) ||
          !role.bindText(1, manifest.id) || !role.bindText(2, manifest.role) ||
          role.step() != storage::SqliteStatement::Step::Done)
        break;
    }
    success = impl_->database.exec("COMMIT;");
  } while (false);
  if (!success) {
    impl_->database.exec("ROLLBACK;");
    error_ = impl_->database.lastError();
    return false;
  }
  if (installed)
    return resolve(manifest.id.c_str(), *installed);
  return true;
}

bool AppRepository::resolve(const char *app_id, AppRecord &record) {
  error_.clear();
  storage::SqliteStatement statement;
  if (!impl_->database.prepare(
          "SELECT a.app_id,a.display_name,v.version,a.role,v.package_path,"
          "v.package_digest,a.enabled FROM applications a JOIN app_versions v "
          "ON"
          " v.app_id=a.app_id AND v.version=a.active_version"
          " WHERE a.app_id=?;",
          statement) ||
      !statement.bindText(1, app_id ? app_id : "")) {
    error_ = impl_->database.lastError();
    return false;
  }
  if (statement.step() != storage::SqliteStatement::Step::Row ||
      !parseRecord(statement, record)) {
    error_ = "application is not installed: " +
             std::string(app_id ? app_id : "(null)");
    return false;
  }
  if (!loadGrantedPermissions(impl_->database, record) ||
      !loadHandlers(impl_->database, record)) {
    error_ = impl_->database.lastError();
    return false;
  }
  return true;
}

bool AppRepository::list(std::vector<AppRecord> &records) {
  records.clear();
  storage::SqliteStatement statement;
  if (!impl_->database.prepare(
          "SELECT a.app_id,a.display_name,v.version,a.role,v.package_path,"
          "v.package_digest,a.enabled FROM applications a JOIN app_versions v "
          "ON"
          " v.app_id=a.app_id AND v.version=a.active_version"
          " ORDER BY a.app_id;",
          statement)) {
    error_ = impl_->database.lastError();
    return false;
  }
  while (true) {
    const auto result = statement.step();
    if (result == storage::SqliteStatement::Step::Done)
      return true;
    AppRecord record;
    if (result != storage::SqliteStatement::Step::Row ||
        !parseRecord(statement, record) ||
        !loadGrantedPermissions(impl_->database, record) ||
        !loadHandlers(impl_->database, record)) {
      error_ = impl_->database.lastError();
      return false;
    }
    records.push_back(std::move(record));
  }
}

bool AppRepository::uninstall(const char *app_id) {
  error_.clear();
  AppRecord record;
  if (!resolve(app_id, record))
    return false;
  if (!impl_->database.exec("BEGIN IMMEDIATE;")) {
    error_ = impl_->database.lastError();
    return false;
  }
  bool success = false;
  do {
    constexpr const char *tables[] = {
        "app_permissions", "app_handlers", "app_roles",
        "app_state",       "app_versions", "applications",
    };
    bool rows_removed = true;
    for (const char *table : tables) {
      storage::SqliteStatement statement;
      const std::string sql =
          std::string("DELETE FROM ") + table + " WHERE app_id=?;";
      if (!impl_->database.prepare(sql.c_str(), statement) ||
          !statement.bindText(1, record.manifest.id) ||
          statement.step() != storage::SqliteStatement::Step::Done) {
        rows_removed = false;
        break;
      }
    }
    if (!rows_removed)
      break;
    success = impl_->database.exec("COMMIT;");
  } while (false);
  if (!success) {
    impl_->database.exec("ROLLBACK;");
    error_ = impl_->database.lastError();
    return false;
  }

  const std::string package_root =
      join(join(data_root_, "packages"), record.manifest.id);
  const std::string cache_root =
      join(join(data_root_, "cache/aot"), record.package_digest);
  const std::string user_data = join(
      join(join(join(data_root_, "users"), "0"), "wasm"), record.manifest.id);
  if (!storage::removeTree(package_root, error_) ||
      !storage::removeTree(cache_root, error_) ||
      !storage::removeTree(user_data, error_)) {
    error_ = "application registry removed, but cleanup failed: " + error_;
    return false;
  }
  return true;
}

bool AppRepository::prepareLaunch(const char *app_id, AppLaunch &launch) {
  AppRecord record;
  if (!resolve(app_id, record))
    return false;
  if (!record.enabled) {
    error_ = "application is disabled: " + record.manifest.id;
    return false;
  }
  ZipArchive archive;
  if (!archive.open(record.package_path.c_str())) {
    error_ = archive.lastError();
    return false;
  }
  std::string entrypoint;
  if (archive.find(kAotModulePath))
    entrypoint = kAotModulePath;
  else if (archive.find(kWasmModulePath))
    entrypoint = kWasmModulePath;
  else {
    error_ = "active package has no executable entry";
    return false;
  }
  const std::string cache_directory =
      join(join(data_root_, "cache/aot"), record.package_digest);
  if (!storage::ensureDirectory(cache_directory, 0700, error_))
    return false;
  const std::string extension =
      entrypoint.size() >= 4 &&
              entrypoint.substr(entrypoint.size() - 4) == ".aot"
          ? ".aot"
      : entrypoint.size() >= 5 &&
              entrypoint.substr(entrypoint.size() - 5) == ".wasm"
          ? ".wasm"
          : ".entry";
  const std::string executable = join(cache_directory, "app" + extension);
  struct stat status = {};
  if (stat(executable.c_str(), &status) != 0 || !S_ISREG(status.st_mode)) {
    std::vector<uint8_t> bytes;
    if (!archive.read(entrypoint.c_str(), bytes)) {
      error_ = archive.lastError();
      return false;
    }
    if (!storage::writeFileAtomic(executable, bytes.data(), bytes.size(), 0500,
                                  error_))
      return false;
  }

  const std::string asset_directory = join(cache_directory, "assets");
  if (!storage::ensureDirectory(asset_directory, 0700, error_))
    return false;
  for (const ZipEntry &entry : archive.entries()) {
    if (entry.name.rfind(kAssetPrefix, 0) != 0 ||
        entry.name.size() == sizeof(kAssetPrefix) - 1 ||
        entry.name.back() == '/')
      continue;
    const std::string relative = entry.name.substr(sizeof(kAssetPrefix) - 1);
    const std::string destination = join(asset_directory, relative);
    struct stat asset_status = {};
    if (stat(destination.c_str(), &asset_status) == 0 &&
        S_ISREG(asset_status.st_mode) &&
        static_cast<uint64_t>(asset_status.st_size) == entry.uncompressed_size)
      continue;
    std::vector<uint8_t> bytes;
    if (!archive.readEntry(entry, bytes, kMaximumAssetBytes) ||
        !storage::writeFileAtomic(destination, bytes.data(), bytes.size(), 0400,
                                  error_)) {
      if (error_.empty())
        error_ = archive.lastError();
      return false;
    }
  }

  const std::string data_directory = join(
      join(join(join(data_root_, "users"), "0"), "wasm"), record.manifest.id);
  if (!storage::ensureDirectory(data_directory, 0700, error_))
    return false;
  launch.app = std::move(record);
  launch.executable_path = executable;
  launch.entrypoint = entrypoint;
  launch.data_directory = data_directory;
  launch.cache_directory = cache_directory;
  launch.asset_directory = asset_directory;
  return true;
}

} // namespace oos::apps
