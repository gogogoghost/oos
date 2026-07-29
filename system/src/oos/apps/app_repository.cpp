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
  const char *package_kind = statement.columnText(3);
  const char *runtime_kind = statement.columnText(4);
  const char *api_profile = statement.columnText(5);
  const char *entrypoint = statement.columnText(6);
  const char *fallback = statement.columnText(7);
  const char *role = statement.columnText(8);
  const char *package_path = statement.columnText(11);
  const char *digest = statement.columnText(12);
  if (!id || !name || !version || !package_kind || !runtime_kind ||
      !api_profile || !entrypoint || !package_path || !digest)
    return false;

  AppRecord parsed;
  parsed.manifest.format = 1;
  parsed.manifest.id = id;
  parsed.manifest.name = name;
  parsed.manifest.version = version;
  parsed.manifest.package_kind =
      std::strcmp(package_kind, "oos-wasm-v1") == 0 ? PackageKind::OosWasmV1
      : std::strcmp(package_kind, "kaios-2.5") == 0 ? PackageKind::KaiOs25
                                                    : PackageKind::KaiOs3;
  parsed.manifest.runtime_kind = std::strcmp(runtime_kind, "wamr") == 0
                                     ? RuntimeKind::Wamr
                                     : RuntimeKind::Wpe;
  parsed.manifest.api_profile = api_profile;
  parsed.manifest.entrypoint = entrypoint;
  parsed.manifest.fallback_entrypoint = fallback ? fallback : "";
  parsed.manifest.role = role ? role : "";
  parsed.manifest.stack_bytes = static_cast<uint32_t>(statement.columnInt64(9));
  parsed.manifest.heap_bytes = static_cast<uint32_t>(statement.columnInt64(10));
  parsed.package_path = package_path;
  parsed.package_digest = digest;
  parsed.enabled = statement.columnInt64(13) != 0;
  record = std::move(parsed);
  return true;
}

bool loadGrantedPermissions(storage::SqliteDatabase &database,
                            AppRecord &record) {
  storage::SqliteStatement permissions;
  if (!database.prepare(
          "SELECT permission FROM app_permissions"
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
      "system",      "runtime",        "packages",        "users/0/wasm",
      "users/0/web", "cache/aot",      "cache/web",       "staging",
      "tmp",         "media/internal", "media/removable",
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
  storage::SqliteStatement schema_version;
  if (!impl_->database.prepare("PRAGMA user_version;", schema_version) ||
      schema_version.step() != storage::SqliteStatement::Step::Row) {
    error_ = impl_->database.lastError();
    return false;
  }
  const int64_t version = schema_version.columnInt64(0);
  if (version > 1) {
    error_ = "application registry schema is newer than this OOS runtime";
    return false;
  }
  if (!impl_->database.exec(
          "CREATE TABLE IF NOT EXISTS applications("
          " app_id TEXT PRIMARY KEY, display_name TEXT NOT NULL,"
          " active_version TEXT NOT NULL, package_kind TEXT NOT NULL,"
          " runtime_kind TEXT NOT NULL, api_profile TEXT NOT NULL,"
          " role TEXT NOT NULL DEFAULT '', enabled INTEGER NOT NULL DEFAULT 1,"
          " trust_level TEXT NOT NULL DEFAULT 'unverified',"
          " installed_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);"
          "CREATE TABLE IF NOT EXISTS app_versions("
          " app_id TEXT NOT NULL, version TEXT NOT NULL,"
          " package_path TEXT NOT NULL, package_digest TEXT NOT NULL,"
          " entrypoint TEXT NOT NULL, fallback_entrypoint TEXT NOT NULL,"
          " stack_bytes INTEGER NOT NULL, heap_bytes INTEGER NOT NULL,"
          " installed_at INTEGER NOT NULL,"
          " PRIMARY KEY(app_id, version));"
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
          " last_started_at INTEGER, last_exit_code INTEGER);")) {
    error_ = impl_->database.lastError();
    return false;
  }
  if (version == 0 && !impl_->database.exec("PRAGMA user_version=1;")) {
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
  const char *manifest_name = nullptr;
  PackageKind detected_kind = PackageKind::OosWasmV1;
  if (archive.find("oos-manifest.json")) {
    manifest_name = "oos-manifest.json";
  } else if (archive.find("manifest.webapp")) {
    manifest_name = "manifest.webapp";
    detected_kind = PackageKind::KaiOs25;
  } else if (archive.find("manifest.webmanifest")) {
    manifest_name = "manifest.webmanifest";
    detected_kind = PackageKind::KaiOs3;
  } else {
    error_ = "application ZIP has no supported manifest";
    return false;
  }
  std::vector<uint8_t> manifest_bytes;
  if (!archive.read(manifest_name, manifest_bytes, kMaximumManifestBytes)) {
    error_ = archive.lastError();
    return false;
  }
  const std::string manifest_json(manifest_bytes.begin(), manifest_bytes.end());
  if (std::strcmp(manifest_name, "oos-manifest.json") == 0) {
    if (!parseAppManifest(manifest_json, manifest, error_))
      return false;
    if (!options.app_id.empty() && options.app_id != manifest.id) {
      error_ = "explicit application id does not match oos-manifest.json";
      return false;
    }
  } else if (options.app_id.empty()) {
    error_ = "KaiOS application ZIP requires an explicit application id";
    return false;
  } else if (!parseKaiOsManifest(manifest_json, detected_kind, options.app_id,
                                 manifest, error_)) {
    return false;
  }
  if (!archive.find(manifest.entrypoint.c_str()) &&
      (manifest.fallback_entrypoint.empty() ||
       !archive.find(manifest.fallback_entrypoint.c_str()))) {
    error_ = "application package does not contain a configured entrypoint";
    return false;
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
            "package_digest,entrypoint,fallback_entrypoint,stack_bytes,"
            "heap_bytes,installed_at) "
            "VALUES(?,?,?,?,?,?,?,?,strftime('%s','now'))"
            " ON CONFLICT(app_id,version) DO UPDATE SET"
            " package_path=excluded.package_path,"
            " package_digest=excluded.package_digest,"
            " entrypoint=excluded.entrypoint,"
            " fallback_entrypoint=excluded.fallback_entrypoint,"
            " stack_bytes=excluded.stack_bytes,heap_bytes=excluded.heap_bytes,"
            " installed_at=excluded.installed_at;",
            version) ||
        !version.bindText(1, manifest.id) ||
        !version.bindText(2, manifest.version) ||
        !version.bindText(3, destination) || !version.bindText(4, digest) ||
        !version.bindText(5, manifest.entrypoint) ||
        !version.bindText(6, manifest.fallback_entrypoint) ||
        !version.bindInt64(7, manifest.stack_bytes) ||
        !version.bindInt64(8, manifest.heap_bytes) ||
        version.step() != storage::SqliteStatement::Step::Done)
      break;

    storage::SqliteStatement app;
    if (!impl_->database.prepare(
            "INSERT INTO applications(app_id,display_name,active_version,"
            "package_kind,runtime_kind,api_profile,role,installed_at,updated_"
            "at)"
            " VALUES(?,?,?,?,?,?,?,strftime('%s','now'),strftime('%s','now'))"
            " ON CONFLICT(app_id) DO UPDATE SET"
            " display_name=excluded.display_name,"
            " active_version=excluded.active_version,"
            " package_kind=excluded.package_kind,"
            " runtime_kind=excluded.runtime_kind,"
            " api_profile=excluded.api_profile,role=excluded.role,"
            " updated_at=excluded.updated_at;",
            app) ||
        !app.bindText(1, manifest.id) || !app.bindText(2, manifest.name) ||
        !app.bindText(3, manifest.version) ||
        !app.bindText(4, packageKindName(manifest.package_kind)) ||
        !app.bindText(5, runtimeKindName(manifest.runtime_kind)) ||
        !app.bindText(6, manifest.api_profile) ||
        !app.bindText(7, manifest.role) ||
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
          "SELECT a.app_id,a.display_name,v.version,a.package_kind,"
          "a.runtime_kind,a.api_profile,v.entrypoint,v.fallback_entrypoint,"
          "a.role,v.stack_bytes,v.heap_bytes,v.package_path,v.package_digest,"
          "a.enabled FROM applications a JOIN app_versions v ON"
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
  if (!loadGrantedPermissions(impl_->database, record)) {
    error_ = impl_->database.lastError();
    return false;
  }
  return true;
}

bool AppRepository::list(std::vector<AppRecord> &records) {
  records.clear();
  storage::SqliteStatement statement;
  if (!impl_->database.prepare(
          "SELECT a.app_id,a.display_name,v.version,a.package_kind,"
          "a.runtime_kind,a.api_profile,v.entrypoint,v.fallback_entrypoint,"
          "a.role,v.stack_bytes,v.heap_bytes,v.package_path,v.package_digest,"
          "a.enabled FROM applications a JOIN app_versions v ON"
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
        !loadGrantedPermissions(impl_->database, record)) {
      error_ = impl_->database.lastError();
      return false;
    }
    records.push_back(std::move(record));
  }
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
  std::string entrypoint = record.manifest.entrypoint;
  if (!archive.find(entrypoint.c_str()))
    entrypoint = record.manifest.fallback_entrypoint;
  if (entrypoint.empty() || !archive.find(entrypoint.c_str())) {
    error_ = "active package entrypoint is unavailable";
    return false;
  }
  const std::string cache_directory =
      record.manifest.runtime_kind == RuntimeKind::Wamr
          ? join(join(data_root_, "cache/aot"), record.package_digest)
          : join(join(join(data_root_, "cache/web"), record.manifest.id),
                 record.package_digest);
  if (!storage::ensureDirectory(cache_directory, 0700, error_))
    return false;
  if (record.manifest.runtime_kind == RuntimeKind::Wpe) {
    const std::string data_directory = join(
        join(join(join(data_root_, "users"), "0"), "web"), record.manifest.id);
    if (!storage::ensureDirectory(data_directory, 0700, error_))
      return false;
    launch.app = std::move(record);
    launch.executable_path = launch.app.package_path;
    launch.entrypoint = entrypoint;
    launch.data_directory = data_directory;
    launch.cache_directory = cache_directory;
    return true;
  }
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

  const std::string runtime =
      record.manifest.runtime_kind == RuntimeKind::Wamr ? "wasm" : "web";
  const std::string data_directory = join(
      join(join(join(data_root_, "users"), "0"), runtime), record.manifest.id);
  if (!storage::ensureDirectory(data_directory, 0700, error_))
    return false;
  launch.app = std::move(record);
  launch.executable_path = executable;
  launch.entrypoint = entrypoint;
  launch.data_directory = data_directory;
  launch.cache_directory = cache_directory;
  return true;
}

} // namespace oos::apps
