#include "oos/services/system_service.h"

#include "oos/apps/app_repository.h"
#include "oos/apps/json.h"
#include "oos/storage/filesystem.h"
#include "oos/storage/sqlite.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>

namespace oos::services {
namespace {

constexpr size_t kMaximumPayloadBytes = 256 * 1024;
constexpr size_t kMaximumNameBytes = 128;
constexpr size_t kMaximumRecordBytes = 240 * 1024;
constexpr int64_t kMaximumPollEvents = 128;

int64_t wallClockMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string join(const std::string &left, const char *right) {
  return left + (left.empty() || left.back() == '/' ? "" : "/") + right;
}

void appendJsonString(std::string &output, const std::string &value) {
  constexpr char hex[] = "0123456789abcdef";
  output.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
    case '"': output += "\\\""; break;
    case '\\': output += "\\\\"; break;
    case '\b': output += "\\b"; break;
    case '\f': output += "\\f"; break;
    case '\n': output += "\\n"; break;
    case '\r': output += "\\r"; break;
    case '\t': output += "\\t"; break;
    default:
      if (character < 0x20) {
        output += "\\u00";
        output.push_back(hex[character >> 4]);
        output.push_back(hex[character & 0x0f]);
      } else {
        output.push_back(static_cast<char>(character));
      }
    }
  }
  output.push_back('"');
}

bool fieldString(const apps::JsonValue &root, const char *name,
                 std::string &value, size_t maximum = kMaximumRecordBytes) {
  const apps::JsonValue *field = root.get(name);
  if (!field || !field->isString() || field->stringValue().size() > maximum)
    return false;
  value = field->stringValue();
  return true;
}

bool fieldInteger(const apps::JsonValue &root, const char *name, int64_t &value,
                  int64_t minimum = std::numeric_limits<int64_t>::min(),
                  int64_t maximum = std::numeric_limits<int64_t>::max()) {
  const apps::JsonValue *field = root.get(name);
  if (!field || !field->isNumber() || field->integerValue() < minimum ||
      field->integerValue() > maximum)
    return false;
  value = field->integerValue();
  return true;
}

bool fieldBoolean(const apps::JsonValue &root, const char *name, bool &value) {
  const apps::JsonValue *field = root.get(name);
  if (!field || !field->isBoolean())
    return false;
  value = field->booleanValue();
  return true;
}

bool validJson(const std::string &encoded) {
  apps::JsonValue value;
  std::string error;
  return encoded.size() <= kMaximumRecordBytes &&
         apps::parseJson(encoded, value, error);
}

bool hasPermission(const std::vector<std::string> &permissions,
                   std::string_view name) {
  return std::find(permissions.begin(), permissions.end(), name) !=
         permissions.end();
}

bool hasPermissionPrefix(const std::vector<std::string> &permissions,
                         std::string_view prefix) {
  return std::any_of(permissions.begin(), permissions.end(),
                     [prefix](const std::string &permission) {
                       return permission.compare(0, prefix.size(), prefix) == 0;
                     });
}

bool hasScopedPermission(const std::vector<std::string> &permissions,
                         std::string_view name) {
  std::string prefix(name);
  prefix.push_back(':');
  return hasPermissionPrefix(permissions, prefix);
}

bool hasReadAccess(const std::vector<std::string> &permissions,
                   std::string_view name) {
  const std::string read = std::string(name) + ":read";
  const std::string write = std::string(name) + ":write";
  return hasPermission(permissions, read) || hasPermission(permissions, write) ||
         (hasPermission(permissions, name) &&
          !hasScopedPermission(permissions, name));
}

bool hasCreateAccess(const std::vector<std::string> &permissions,
                     std::string_view name) {
  const std::string create = std::string(name) + ":create";
  const std::string write = std::string(name) + ":write";
  return hasPermission(permissions, create) || hasPermission(permissions, write) ||
         (hasPermission(permissions, name) &&
          !hasScopedPermission(permissions, name));
}

bool hasWriteAccess(const std::vector<std::string> &permissions,
                    std::string_view name) {
  const std::string write = std::string(name) + ":write";
  return hasPermission(permissions, write) ||
         (hasPermission(permissions, name) &&
          !hasScopedPermission(permissions, name));
}

bool subscriptionAllowed(const std::vector<std::string> &permissions,
                         std::string_view topic) {
  if (hasPermission(permissions, std::string("system-message:") +
                                     std::string(topic)))
    return true;
  if (topic.compare(0, 8, "setting:") == 0)
    return hasReadAccess(permissions, "settings");
  if (topic == "contacts")
    return hasReadAccess(permissions, "contacts");
  if (topic == "notification")
    return hasPermission(permissions, "desktop-notification") ||
           hasPermission(permissions, "notification");
  if (topic.compare(0, 14, "accessibility:") == 0)
    return true;
  return false;
}

bool permissionFor(const std::vector<std::string> &permissions,
                   const std::string &service, const std::string &operation,
                   bool system_authority) {
  if (system_authority)
    return true;
  if (service == "alarms")
    return hasPermission(permissions, "alarms");
  if (service == "settings")
    return operation == "get" || operation == "get-batch"
               ? hasReadAccess(permissions, "settings")
           : operation == "set" ? hasCreateAccess(permissions, "settings")
                                : hasWriteAccess(permissions, "settings");
  if (service == "notifications")
    return hasPermission(permissions, "desktop-notification") ||
           hasPermission(permissions, "notification");
  if (service == "contacts")
    return operation == "get" || operation == "list"
               ? hasReadAccess(permissions, "contacts")
           : operation == "add" ? hasCreateAccess(permissions, "contacts")
                                : hasWriteAccess(permissions, "contacts");
  if (service == "system-messages")
    return operation == "poll" || operation == "subscribe";
  if (service == "activities")
    return operation == "start" || operation == "status" ||
           operation == "cancel";
  if (service == "audio-policy")
    return operation == "request" ||
           hasPermission(permissions, "audio-channel-content") ||
           hasPermissionPrefix(permissions, "audio-channel-") ||
           hasPermission(permissions, "volume-control");
  if (service == "accessibility")
    return operation == "get" || operation == "get-batch";
  if (service == "applications")
    return hasPermission(permissions, "apps-management");
  if (service == "input-method")
    return hasPermission(permissions, "input") ||
           hasPermission(permissions, "input-manage");
  if (service == "time")
    return operation == "get"
               ? hasPermission(permissions, "system-time") ||
                     hasPermission(permissions, "system-time:read") ||
                     hasPermission(permissions, "system-time:write")
               : hasPermission(permissions, "system-time") ||
                     hasPermission(permissions, "system-time:write");
  return false;
}

bool prepare(storage::SqliteDatabase &database, const char *sql,
             storage::SqliteStatement &statement, std::string &error) {
  if (database.prepare(sql, statement))
    return true;
  error = database.lastError();
  return false;
}

} // namespace

class SystemServiceHub::Impl {
public:
  Impl(std::string root, apps::AppRepository *repository)
      : data_root(std::move(root)), applications(repository) {}

  bool initialize() {
    error.clear();
    const std::string system_directory = join(data_root, "system");
    if (!storage::ensureDirectory(system_directory, 0700, error) ||
        !database.open(join(system_directory, "services.sqlite3"))) {
      if (error.empty())
        error = database.lastError();
      return false;
    }
    if (!database.exec(
            "CREATE TABLE IF NOT EXISTS service_state("
            " namespace TEXT NOT NULL,key TEXT NOT NULL,value_json TEXT NOT NULL,"
            " revision INTEGER NOT NULL DEFAULT 1,updated_at INTEGER NOT NULL,"
            " PRIMARY KEY(namespace,key));"
            "CREATE TABLE IF NOT EXISTS service_records("
            " domain TEXT NOT NULL,owner TEXT NOT NULL,record_id INTEGER NOT NULL,"
            " value_json TEXT NOT NULL,created_at INTEGER NOT NULL,"
            " updated_at INTEGER NOT NULL,PRIMARY KEY(domain,owner,record_id));"
            "CREATE INDEX IF NOT EXISTS service_records_domain_owner"
            " ON service_records(domain,owner,record_id);"
            "CREATE TABLE IF NOT EXISTS system_subscriptions("
            " app_id TEXT NOT NULL,topic TEXT NOT NULL,"
            " PRIMARY KEY(app_id,topic));"
            "CREATE TABLE IF NOT EXISTS system_events("
            " sequence INTEGER PRIMARY KEY AUTOINCREMENT,target_app TEXT NOT NULL,"
            " topic TEXT NOT NULL,payload_json TEXT NOT NULL,"
            " created_at INTEGER NOT NULL,consumed INTEGER NOT NULL DEFAULT 0);"
            "CREATE INDEX IF NOT EXISTS system_events_target"
            " ON system_events(target_app,consumed,sequence);")) {
      error = database.lastError();
      return false;
    }
    return true;
  }

  int stateRequest(const apps::JsonValue &arguments,
                   const std::string &name_space, const std::string &operation,
                   std::string &response) {
    if (operation == "clear") {
      storage::SqliteStatement statement;
      if (!prepare(database, "DELETE FROM service_state WHERE namespace=?;",
                   statement, error) || !statement.bindText(1, name_space) ||
          statement.step() != storage::SqliteStatement::Step::Done)
        return failIo();
      response = "null";
      return 0;
    }
    if (operation == "get-batch") {
      const apps::JsonValue *names = arguments.get("names");
      if (!names || !names->isArray() || names->arrayValue().size() > 128)
        return -EINVAL;
      response = "{";
      bool first = true;
      for (const apps::JsonValue &entry : names->arrayValue()) {
        if (!entry.isString() || entry.stringValue().empty() ||
            entry.stringValue().size() > kMaximumNameBytes)
          return -EINVAL;
        storage::SqliteStatement statement;
        if (!prepare(database,
                     "SELECT value_json FROM service_state"
                     " WHERE namespace=? AND key=?;",
                     statement, error) ||
            !statement.bindText(1, name_space) ||
            !statement.bindText(2, entry.stringValue()))
          return failIo();
        const auto row = statement.step();
        if (row != storage::SqliteStatement::Step::Row &&
            row != storage::SqliteStatement::Step::Done)
          return failIo();
        if (!first)
          response.push_back(',');
        first = false;
        appendJsonString(response, entry.stringValue());
        response.push_back(':');
        response += row == storage::SqliteStatement::Step::Row
                        ? statement.columnText(0)
                        : "null";
      }
      response.push_back('}');
      return 0;
    }
    std::string key;
    if (!fieldString(arguments, "name", key, kMaximumNameBytes) || key.empty())
      return -EINVAL;
    if (operation == "remove") {
      storage::SqliteStatement statement;
      if (!prepare(database,
                   "DELETE FROM service_state WHERE namespace=? AND key=?;",
                   statement, error) || !statement.bindText(1, name_space) ||
          !statement.bindText(2, key) ||
          statement.step() != storage::SqliteStatement::Step::Done)
        return failIo();
      response = "null";
      return 0;
    }
    if (operation == "get") {
      storage::SqliteStatement statement;
      if (!prepare(database,
                   "SELECT value_json,revision FROM service_state"
                   " WHERE namespace=? AND key=?;",
                   statement, error) || !statement.bindText(1, name_space) ||
          !statement.bindText(2, key))
        return failIo();
      const auto row = statement.step();
      if (row == storage::SqliteStatement::Step::Done) {
        response = "{\"found\":false}";
        return 0;
      }
      if (row != storage::SqliteStatement::Step::Row)
        return failIo();
      response = "{\"found\":true,\"value\":";
      response += statement.columnText(0);
      response += ",\"revision\":" +
                  std::to_string(statement.columnInt64(1)) + "}";
      return 0;
    }
    if (operation != "set")
      return -ENOSYS;
    std::string value;
    if (!fieldString(arguments, "value", value) || !validJson(value))
      return -EINVAL;
    storage::SqliteStatement statement;
    if (!prepare(database,
                 "INSERT INTO service_state(namespace,key,value_json,updated_at)"
                 " VALUES(?,?,?,?) ON CONFLICT(namespace,key) DO UPDATE SET"
                 " value_json=excluded.value_json,revision=revision+1,"
                 " updated_at=excluded.updated_at;",
                 statement, error) || !statement.bindText(1, name_space) ||
        !statement.bindText(2, key) || !statement.bindText(3, value) ||
        !statement.bindInt64(4, wallClockMilliseconds()) ||
        statement.step() != storage::SqliteStatement::Step::Done)
      return failIo();
    response = "null";
    return 0;
  }

  int recordRequest(const apps::JsonValue &arguments, const std::string &domain,
                    const std::string &owner, const std::string &operation,
                    std::string &response) {
    if (operation == "clear") {
      storage::SqliteStatement statement;
      if (!prepare(database,
                   "DELETE FROM service_records WHERE domain=? AND owner=?;",
                   statement, error) || !statement.bindText(1, domain) ||
          !statement.bindText(2, owner) ||
          statement.step() != storage::SqliteStatement::Step::Done)
        return failIo();
      response = "null";
      return 0;
    }
    if (operation == "list") {
      storage::SqliteStatement statement;
      if (!prepare(database,
                   "SELECT record_id,value_json,created_at,updated_at FROM"
                   " service_records WHERE domain=? AND owner=?"
                   " ORDER BY record_id;",
                   statement, error) || !statement.bindText(1, domain) ||
          !statement.bindText(2, owner))
        return failIo();
      response = "[";
      bool first = true;
      while (true) {
        const auto row = statement.step();
        if (row == storage::SqliteStatement::Step::Done)
          break;
        if (row != storage::SqliteStatement::Step::Row)
          return failIo();
        if (!first)
          response.push_back(',');
        first = false;
        response += "{\"id\":" + std::to_string(statement.columnInt64(0)) +
                    ",\"value\":" + statement.columnText(1) +
                    ",\"createdAt\":" +
                    std::to_string(statement.columnInt64(2)) +
                    ",\"updatedAt\":" +
                    std::to_string(statement.columnInt64(3)) + "}";
      }
      response.push_back(']');
      return 0;
    }
    int64_t id = 0;
    if (operation == "get" || operation == "remove" || operation == "put") {
      if (!fieldInteger(arguments, "id", id, 1))
        return -EINVAL;
    }
    if (operation == "get") {
      storage::SqliteStatement statement;
      if (!prepare(database,
                   "SELECT value_json FROM service_records"
                   " WHERE domain=? AND owner=? AND record_id=?;",
                   statement, error) || !statement.bindText(1, domain) ||
          !statement.bindText(2, owner) || !statement.bindInt64(3, id))
        return failIo();
      const auto row = statement.step();
      if (row == storage::SqliteStatement::Step::Done) {
        response = "null";
        return 0;
      }
      if (row != storage::SqliteStatement::Step::Row)
        return failIo();
      response = statement.columnText(0);
      return 0;
    }
    if (operation == "remove") {
      storage::SqliteStatement statement;
      if (!prepare(database,
                   "DELETE FROM service_records WHERE domain=? AND owner=?"
                   " AND record_id=?;",
                   statement, error) || !statement.bindText(1, domain) ||
          !statement.bindText(2, owner) || !statement.bindInt64(3, id) ||
          statement.step() != storage::SqliteStatement::Step::Done)
        return failIo();
      response = "true";
      return 0;
    }
    if (operation != "add" && operation != "put")
      return -ENOSYS;
    std::string value;
    if (!fieldString(arguments, "value", value) || !validJson(value))
      return -EINVAL;
    const int64_t now = wallClockMilliseconds();
    storage::SqliteStatement statement;
    if (operation == "add") {
      if (!prepare(database,
                   "INSERT INTO service_records(domain,owner,record_id,"
                   " value_json,created_at,updated_at) SELECT ?,?,"
                   " COALESCE(MAX(record_id),0)+1,?,?,? FROM service_records"
                   " WHERE domain=? AND owner=?;",
                   statement, error) || !statement.bindText(1, domain) ||
          !statement.bindText(2, owner) || !statement.bindText(3, value) ||
          !statement.bindInt64(4, now) || !statement.bindInt64(5, now) ||
          !statement.bindText(6, domain) || !statement.bindText(7, owner) ||
          statement.step() != storage::SqliteStatement::Step::Done)
        return failIo();
      const int64_t row_id = database.lastInsertRowId();
      storage::SqliteStatement inserted;
      if (!prepare(database,
                   "SELECT record_id FROM service_records WHERE rowid=?;",
                   inserted, error) || !inserted.bindInt64(1, row_id) ||
          inserted.step() != storage::SqliteStatement::Step::Row)
        return failIo();
      id = inserted.columnInt64(0);
    } else {
      if (!prepare(database,
                   "UPDATE service_records SET value_json=?,updated_at=?"
                   " WHERE domain=? AND owner=? AND record_id=?;",
                   statement, error) || !statement.bindText(1, value) ||
          !statement.bindInt64(2, now) || !statement.bindText(3, domain) ||
          !statement.bindText(4, owner) || !statement.bindInt64(5, id) ||
          statement.step() != storage::SqliteStatement::Step::Done)
        return failIo();
      if (database.changes() == 0)
        return -ENOENT;
    }
    response = std::to_string(id);
    return 0;
  }

  int listAllRecords(const std::string &domain, std::string &response) {
    storage::SqliteStatement statement;
    if (!prepare(database,
                 "SELECT owner,record_id,value_json,created_at,updated_at FROM"
                 " service_records WHERE domain=? ORDER BY owner,record_id;",
                 statement, error) || !statement.bindText(1, domain))
      return failIo();
    response = "[";
    bool first = true;
    while (true) {
      const auto row = statement.step();
      if (row == storage::SqliteStatement::Step::Done)
        break;
      if (row != storage::SqliteStatement::Step::Row)
        return failIo();
      if (!first)
        response.push_back(',');
      first = false;
      response += "{\"owner\":";
      appendJsonString(response, statement.columnText(0));
      response += ",\"id\":" + std::to_string(statement.columnInt64(1)) +
                  ",\"value\":" + statement.columnText(2) +
                  ",\"createdAt\":" +
                  std::to_string(statement.columnInt64(3)) +
                  ",\"updatedAt\":" +
                  std::to_string(statement.columnInt64(4)) + "}";
    }
    response.push_back(']');
    return 0;
  }

  int eventRequest(const std::string &app_id,
                   const std::vector<std::string> &permissions,
                   const apps::JsonValue &arguments,
                   const std::string &operation, std::string &response,
                   bool system_authority) {
    if (operation == "subscribe") {
      std::string topic;
      if (!fieldString(arguments, "topic", topic, kMaximumNameBytes) ||
          topic.empty())
        return -EINVAL;
      if (!system_authority && !subscriptionAllowed(permissions, topic))
        return -EACCES;
      storage::SqliteStatement statement;
      if (!prepare(database,
                   "INSERT OR IGNORE INTO system_subscriptions(app_id,topic)"
                   " VALUES(?,?);",
                   statement, error) || !statement.bindText(1, app_id) ||
          !statement.bindText(2, topic) ||
          statement.step() != storage::SqliteStatement::Step::Done)
        return failIo();
      response = "null";
      return 0;
    }
    if (operation == "publish") {
      if (!system_authority)
        return -EACCES;
      std::string target;
      std::string topic;
      std::string payload;
      if (!fieldString(arguments, "targetApp", target, kMaximumNameBytes) ||
          !fieldString(arguments, "topic", topic, kMaximumNameBytes) ||
          !fieldString(arguments, "payload", payload) || !validJson(payload))
        return -EINVAL;
      return enqueue(target, topic, payload, response);
    }
    if (operation != "poll")
      return -ENOSYS;
    int64_t after = 0;
    int64_t limit = 32;
    const apps::JsonValue *after_value = arguments.get("after");
    const apps::JsonValue *limit_value = arguments.get("limit");
    if ((after_value && !fieldInteger(arguments, "after", after, 0)) ||
        (limit_value &&
         !fieldInteger(arguments, "limit", limit, 1, kMaximumPollEvents)))
      return -EINVAL;
    promoteDueAlarms(app_id);
    storage::SqliteStatement statement;
    if (!prepare(database,
                 "SELECT sequence,topic,payload_json,created_at FROM"
                 " system_events WHERE target_app=? AND consumed=0"
                 " AND sequence>?"
                 " ORDER BY sequence LIMIT ?;",
                 statement, error) || !statement.bindText(1, app_id) ||
        !statement.bindInt64(2, after) || !statement.bindInt64(3, limit))
      return failIo();
    response = "[";
    bool first = true;
    int64_t last_sequence = 0;
    while (true) {
      const auto row = statement.step();
      if (row == storage::SqliteStatement::Step::Done)
        break;
      if (row != storage::SqliteStatement::Step::Row)
        return failIo();
      if (!first)
        response.push_back(',');
      first = false;
      last_sequence = statement.columnInt64(0);
      response += "{\"sequence\":" +
                  std::to_string(statement.columnInt64(0)) + ",\"topic\":";
      appendJsonString(response, statement.columnText(1));
      response += ",\"payload\":" + std::string(statement.columnText(2)) +
                  ",\"createdAt\":" +
                  std::to_string(statement.columnInt64(3)) + "}";
    }
    response.push_back(']');
    if (last_sequence > 0) {
      storage::SqliteStatement consumed;
      if (!prepare(database,
                   "UPDATE system_events SET consumed=1 WHERE target_app=?"
                   " AND sequence<=?;",
                   consumed, error) || !consumed.bindText(1, app_id) ||
          !consumed.bindInt64(2, last_sequence) ||
          consumed.step() != storage::SqliteStatement::Step::Done)
        return failIo();
    }
    return 0;
  }

  void broadcast(const std::string &topic, const std::string &payload) {
    storage::SqliteStatement statement;
    if (!database.prepare(
            "SELECT app_id FROM system_subscriptions WHERE topic=?;",
            statement) || !statement.bindText(1, topic))
      return;
    std::vector<std::string> targets;
    while (statement.step() == storage::SqliteStatement::Step::Row)
      targets.emplace_back(statement.columnText(0));
    for (const std::string &target : targets) {
      std::string ignored;
      enqueue(target, topic, payload, ignored);
    }
  }

  int applicationsRequest(const std::string &operation, std::string &response) {
    if (!applications)
      return -ENOSYS;
    std::vector<apps::AppRecord> records;
    if (operation != "list" || !applications->list(records)) {
      error = applications->lastError();
      return operation == "list" ? -EIO : -ENOSYS;
    }
    response = "[";
    for (size_t index = 0; index < records.size(); ++index) {
      if (index)
        response.push_back(',');
      response += "{\"id\":";
      appendJsonString(response, records[index].manifest.id);
      response += ",\"name\":";
      appendJsonString(response, records[index].manifest.name);
      response += ",\"version\":";
      appendJsonString(response, records[index].manifest.version);
      response += ",\"enabled\":";
      response += records[index].enabled ? "true" : "false";
      response += ",\"activities\":[";
      bool first_handler = true;
      for (const apps::AppHandler &handler : records[index].manifest.handlers) {
        if (handler.kind != "activity")
          continue;
        if (!first_handler)
          response.push_back(',');
        first_handler = false;
        appendJsonString(response, handler.value);
      }
      response.push_back(']');
      response.push_back('}');
    }
    response.push_back(']');
    return 0;
  }

  int request(const std::string &app_id,
              const std::vector<std::string> &permissions,
              const std::string &service, const std::string &operation,
              const std::string &payload, std::string &response,
              bool system_authority) {
    error.clear();
    response.clear();
    if (app_id.empty() || service.empty() || operation.empty() ||
        payload.size() > kMaximumPayloadBytes)
      return -EINVAL;
    if (!permissionFor(permissions, service, operation, system_authority))
      return -EACCES;
    apps::JsonValue arguments;
    std::string parse_error;
    if (!apps::parseJson(payload.empty() ? "{}" : payload, arguments,
                         parse_error) || !arguments.isObject()) {
      error = parse_error;
      return -EINVAL;
    }
    if (service == "settings") {
      const int result =
          stateRequest(arguments, "settings", operation, response);
      if (result == 0 && operation == "set") {
        std::string name;
        std::string value;
        if (fieldString(arguments, "name", name, kMaximumNameBytes) &&
            fieldString(arguments, "value", value) && validJson(value)) {
          std::string payload = "{\"name\":";
          appendJsonString(payload, name);
          payload += ",\"value\":" + value + "}";
          broadcast("setting:" + name, payload);
        }
      }
      return result;
    }
    if (service == "audio-policy") {
      if (operation == "request") {
        std::string action;
        if (!fieldString(arguments, "action", action, kMaximumNameBytes) ||
            (action != "VOLUME_UP" && action != "VOLUME_DOWN" &&
             action != "VOLUME_SHOW"))
          return -EINVAL;
        std::string value = "{\"sourceApp\":";
        appendJsonString(value, app_id);
        value += ",\"action\":";
        appendJsonString(value, action);
        value += ",\"requestedAt\":" +
                 std::to_string(wallClockMilliseconds()) + "}";
        std::string wrapped = "{\"value\":";
        appendJsonString(wrapped, value);
        wrapped.push_back('}');
        apps::JsonValue record_arguments;
        std::string parse_error;
        if (!apps::parseJson(wrapped, record_arguments, parse_error))
          return -EINVAL;
        return recordRequest(record_arguments, "audio-requests", "system",
                             "add", response);
      }
      if (system_authority && operation == "list-requests")
        return recordRequest(arguments, "audio-requests", "system", "list",
                             response);
      return stateRequest(arguments, "audio-policy", operation, response);
    }
    if (service == "accessibility") {
      const int result =
          stateRequest(arguments, "accessibility", operation, response);
      if (result == 0 && operation == "set") {
        std::string name;
        std::string value;
        if (fieldString(arguments, "name", name, kMaximumNameBytes) &&
            fieldString(arguments, "value", value) && validJson(value)) {
          std::string payload = "{\"name\":";
          appendJsonString(payload, name);
          payload += ",\"value\":" + value + "}";
          broadcast("accessibility:" + name, payload);
        }
      }
      return result;
    }
    if (service == "input-method")
      return stateRequest(arguments,
                          system_authority ? "input-method"
                                           : "input-method:" + app_id,
                          operation, response);
    if (service == "time") {
      if (operation == "get") {
        response = "{\"timeMs\":" + std::to_string(wallClockMilliseconds()) +
                   "}";
        return 0;
      }
      return stateRequest(arguments, "time-policy", operation, response);
    }
    if (service == "alarms") {
      if (system_authority && operation == "list-all")
        return listAllRecords("alarms", response);
      return alarmRequest(app_id, arguments, operation, response);
    }
    if (service == "notifications") {
      if (system_authority && operation == "list-all")
        return listAllRecords("notifications", response);
      return recordRequest(arguments, "notifications", app_id, operation,
                           response);
    }
    if (service == "contacts") {
      const int result =
          recordRequest(arguments, "contacts", "system", operation, response);
      if (result == 0 && (operation == "add" || operation == "put" ||
                          operation == "remove" || operation == "clear")) {
        std::string payload = "{\"sourceApp\":";
        appendJsonString(payload, app_id);
        payload += ",\"operation\":";
        appendJsonString(payload, operation);
        if (operation == "add")
          payload += ",\"id\":" + response;
        else if (operation == "put" || operation == "remove") {
          int64_t id = 0;
          if (fieldInteger(arguments, "id", id, 1))
            payload += ",\"id\":" + std::to_string(id);
        }
        payload.push_back('}');
        broadcast("contacts", payload);
      }
      return result;
    }
    if (service == "activities")
      return activityRequest(app_id, arguments, operation, response,
                             system_authority);
    if (service == "system-messages")
      return eventRequest(app_id, permissions, arguments, operation, response,
                          system_authority);
    if (service == "applications")
      return applicationsRequest(operation, response);
    return -ENOSYS;
  }

  int alarmRequest(const std::string &app_id,
                   const apps::JsonValue &arguments,
                   const std::string &operation, std::string &response) {
    if (operation == "list")
      return recordRequest(arguments, "alarms", app_id, operation, response);
    if (operation == "remove")
      return recordRequest(arguments, "alarms", app_id, operation, response);
    if (operation != "add")
      return -ENOSYS;
    int64_t date_ms = 0;
    bool ignore_timezone = false;
    std::string data;
    if (!fieldInteger(arguments, "dateMs", date_ms, 0) ||
        !fieldBoolean(arguments, "ignoreTimezone", ignore_timezone) ||
        !fieldString(arguments, "data", data) || !validJson(data))
      return -EINVAL;
    std::string value = "{\"dateMs\":" + std::to_string(date_ms) +
                        ",\"ignoreTimezone\":" +
                        (ignore_timezone ? "true" : "false") +
                        ",\"data\":" + data + "}";
    const std::string encoded = "{\"value\":";
    std::string wrapped = encoded;
    appendJsonString(wrapped, value);
    wrapped.push_back('}');
    apps::JsonValue record_arguments;
    std::string parse_error;
    if (!apps::parseJson(wrapped, record_arguments, parse_error))
      return -EINVAL;
    return recordRequest(record_arguments, "alarms", app_id, "add", response);
  }

  int activityRequest(const std::string &app_id,
                      const apps::JsonValue &arguments,
                      const std::string &operation, std::string &response,
                      bool system_authority) {
    if (operation == "list") {
      if (!system_authority)
        return -EACCES;
      return listAllRecords("activities", response);
    }
    if (operation == "start") {
      std::string name;
      std::string data;
      if (!fieldString(arguments, "name", name, kMaximumNameBytes) ||
          name.empty() || !fieldString(arguments, "data", data) ||
          !validJson(data))
        return -EINVAL;
      std::string value = "{\"sourceApp\":";
      appendJsonString(value, app_id);
      value += ",\"name\":";
      appendJsonString(value, name);
      value += ",\"data\":" + data + ",\"state\":\"pending\"}";
      std::string wrapped = "{\"value\":";
      appendJsonString(wrapped, value);
      wrapped.push_back('}');
      apps::JsonValue record_arguments;
      std::string parse_error;
      if (!apps::parseJson(wrapped, record_arguments, parse_error))
        return -EINVAL;
      return recordRequest(record_arguments, "activities", app_id, "add",
                           response);
    }
    if (operation == "status" || operation == "cancel") {
      return recordRequest(arguments, "activities", app_id,
                           operation == "status" ? "get" : "remove", response);
    }
    if (system_authority && operation == "put") {
      std::string owner;
      if (!fieldString(arguments, "owner", owner, kMaximumNameBytes) ||
          owner.empty())
        return -EINVAL;
      return recordRequest(arguments, "activities", owner, "put", response);
    }
    return -ENOSYS;
  }

  int enqueue(const std::string &target, const std::string &topic,
              const std::string &payload, std::string &response) {
    storage::SqliteStatement statement;
    if (!prepare(database,
                 "INSERT INTO system_events(target_app,topic,payload_json,"
                 " created_at) VALUES(?,?,?,?);",
                 statement, error) || !statement.bindText(1, target) ||
        !statement.bindText(2, topic) || !statement.bindText(3, payload) ||
        !statement.bindInt64(4, wallClockMilliseconds()) ||
        statement.step() != storage::SqliteStatement::Step::Done)
      return failIo();
    response = std::to_string(database.lastInsertRowId());
    return 0;
  }

  void promoteDueAlarms(const std::string &app_id) {
    storage::SqliteStatement statement;
    if (!database.prepare(
            "SELECT record_id,value_json FROM service_records"
            " WHERE domain='alarms' AND owner=? ORDER BY record_id;",
            statement) || !statement.bindText(1, app_id))
      return;
    std::vector<int64_t> due;
    std::vector<std::string> payloads;
    const int64_t now = wallClockMilliseconds();
    while (statement.step() == storage::SqliteStatement::Step::Row) {
      apps::JsonValue alarm;
      std::string parse_error;
      const std::string encoded = statement.columnText(1);
      if (!apps::parseJson(encoded, alarm, parse_error) || !alarm.isObject())
        continue;
      int64_t date_ms = 0;
      if (fieldInteger(alarm, "dateMs", date_ms, 0) && date_ms <= now) {
        due.push_back(statement.columnInt64(0));
        payloads.push_back(encoded);
      }
    }
    for (size_t index = 0; index < due.size(); ++index) {
      std::string ignored;
      enqueue(app_id, "alarm", payloads[index], ignored);
      storage::SqliteStatement remove;
      if (database.prepare(
              "DELETE FROM service_records WHERE domain='alarms'"
              " AND owner=? AND record_id=?;",
              remove) && remove.bindText(1, app_id) &&
          remove.bindInt64(2, due[index]))
        remove.step();
    }
  }

  int failIo() {
    error = database.lastError();
    return -EIO;
  }

  std::string data_root;
  apps::AppRepository *applications = nullptr;
  storage::SqliteDatabase database;
  std::string error;
};

SystemServiceHub::SystemServiceHub(std::string data_root,
                                   apps::AppRepository *applications)
    : impl_(std::make_unique<Impl>(std::move(data_root), applications)) {}

SystemServiceHub::~SystemServiceHub() = default;

bool SystemServiceHub::initialize() { return impl_->initialize(); }

int SystemServiceHub::request(const std::string &app_id,
                              const std::vector<std::string> &permissions,
                              const std::string &service,
                              const std::string &operation,
                              const std::string &payload,
                              std::string &response, bool system_authority) {
  return impl_->request(app_id, permissions, service, operation, payload,
                        response, system_authority);
}

const std::string &SystemServiceHub::lastError() const { return impl_->error; }

} // namespace oos::services
