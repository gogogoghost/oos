#include "oos/storage/app_storage.h"

#include "oos/storage/filesystem.h"

#include <cctype>

namespace oos::storage {

struct AppStorage::StatementSession {
  uint32_t handle = 0;
  std::unique_ptr<SqliteDatabase> database;
  SqliteStatement statement;
};

AppStorage::AppStorage(std::string app_data_directory)
    : data_directory_(std::move(app_data_directory)) {}

AppStorage::~AppStorage() = default;

bool AppStorage::initialize() {
  error_.clear();
  if (!ensureDirectory(data_directory_, 0700, error_) ||
      !ensureDirectory(data_directory_ + "/db", 0700, error_))
    return false;
  if (!kv_.open(data_directory_ + "/kv.sqlite3") ||
      !kv_.exec("CREATE TABLE IF NOT EXISTS kv("
                " key TEXT PRIMARY KEY, value BLOB NOT NULL,"
                " updated_at INTEGER NOT NULL);")) {
    error_ = kv_.lastError();
    return false;
  }
  return true;
}

bool AppStorage::validKey(const std::string &key) const {
  return !key.empty() && key.size() <= 256;
}

bool AppStorage::get(const std::string &key, std::vector<uint8_t> &value,
                     bool &found) {
  found = false;
  value.clear();
  if (!validKey(key)) {
    error_ = "KV key must contain 1-256 bytes";
    return false;
  }
  SqliteStatement statement;
  if (!kv_.prepare("SELECT value FROM kv WHERE key=?;", statement) ||
      !statement.bindText(1, key)) {
    error_ = kv_.lastError();
    return false;
  }
  const auto result = statement.step();
  if (result == SqliteStatement::Step::Done)
    return true;
  if (result != SqliteStatement::Step::Row) {
    error_ = kv_.lastError();
    return false;
  }
  const int size = statement.columnBytes(0);
  const uint8_t *bytes = statement.columnBlob(0);
  if (size < 0 || (size > 0 && !bytes)) {
    error_ = "KV database returned an invalid value";
    return false;
  }
  if (size > 0)
    value.assign(bytes, bytes + size);
  found = true;
  return true;
}

bool AppStorage::set(const std::string &key, const uint8_t *value,
                     size_t size) {
  if (!validKey(key) || size > 4 * 1024 * 1024 || (size > 0 && !value)) {
    error_ = "KV key or value exceeds limits";
    return false;
  }
  SqliteStatement statement;
  if (!kv_.prepare("INSERT INTO kv(key,value,updated_at) "
                   "VALUES(?,?,strftime('%s','now'))"
                   " ON CONFLICT(key) DO UPDATE SET value=excluded.value,"
                   " updated_at=excluded.updated_at;",
                   statement) ||
      !statement.bindText(1, key) || !statement.bindBlob(2, value, size) ||
      statement.step() != SqliteStatement::Step::Done) {
    error_ = kv_.lastError();
    return false;
  }
  return true;
}

bool AppStorage::remove(const std::string &key, bool &removed) {
  removed = false;
  if (!validKey(key)) {
    error_ = "KV key must contain 1-256 bytes";
    return false;
  }
  SqliteStatement statement;
  if (!kv_.prepare("DELETE FROM kv WHERE key=?;", statement) ||
      !statement.bindText(1, key) ||
      statement.step() != SqliteStatement::Step::Done) {
    error_ = kv_.lastError();
    return false;
  }
  removed = kv_.changes() > 0;
  return true;
}

bool AppStorage::clear() {
  if (!kv_.exec("DELETE FROM kv;")) {
    error_ = kv_.lastError();
    return false;
  }
  return true;
}

bool AppStorage::openDatabase(const std::string &name,
                              std::unique_ptr<SqliteDatabase> &database) {
  if (!validDatabaseName(name))
    return false;
  auto opened = std::make_unique<SqliteDatabase>();
  if (!opened->open(data_directory_ + "/db/" + name + ".sqlite3")) {
    error_ = opened->lastError();
    return false;
  }
  database = std::move(opened);
  return true;
}

bool AppStorage::validDatabaseName(const std::string &name) {
  if (name.empty() || name.size() > 64) {
    error_ = "database name must contain 1-64 characters";
    return false;
  }
  for (const unsigned char character : name) {
    if (!std::isalnum(character) && character != '-' && character != '_') {
      error_ = "database name contains unsupported characters";
      return false;
    }
  }
  return true;
}

bool AppStorage::databaseExecute(const std::string &name,
                                 const std::string &sql, uint32_t &changes) {
  changes = 0;
  if (sql.empty() || sql.size() > 64 * 1024) {
    error_ = "SQL statement must contain 1-65536 bytes";
    return false;
  }
  std::unique_ptr<SqliteDatabase> database;
  if (!openDatabase(name, database) || !database->exec(sql.c_str())) {
    if (database)
      error_ = database->lastError();
    return false;
  }
  changes = static_cast<uint32_t>(database->changes());
  return true;
}

bool AppStorage::databasePrepare(const std::string &name,
                                 const std::string &sql, uint32_t &statement) {
  statement = 0;
  if (sql.empty() || sql.size() > 64 * 1024 || statements_.size() >= 16) {
    error_ = statements_.size() >= 16 ? "open SQL statement limit reached"
                                      : "SQL statement exceeds limits";
    return false;
  }
  auto session = std::make_unique<StatementSession>();
  if (!openDatabase(name, session->database) ||
      !session->database->prepare(sql.c_str(), session->statement)) {
    if (session->database)
      error_ = session->database->lastError();
    return false;
  }
  if (next_statement_ == 0)
    next_statement_ = 1;
  session->handle = next_statement_++;
  statement = session->handle;
  statements_.push_back(std::move(session));
  return true;
}

AppStorage::StatementSession *AppStorage::findStatement(uint32_t statement) {
  for (auto &session : statements_) {
    if (session->handle == statement)
      return session.get();
  }
  error_ = "SQL statement handle is invalid";
  return nullptr;
}

bool AppStorage::statementStep(uint32_t statement, SqlRowState &state) {
  StatementSession *session = findStatement(statement);
  if (!session)
    return false;
  const auto result = session->statement.step();
  if (result == SqliteStatement::Step::Error) {
    error_ = session->database->lastError();
    return false;
  }
  state = result == SqliteStatement::Step::Row ? SqlRowState::Row
                                               : SqlRowState::Done;
  return true;
}

bool AppStorage::statementBindNull(uint32_t statement, uint32_t index) {
  StatementSession *session = findStatement(statement);
  if (!session || index == 0 || !session->statement.bindNull(index)) {
    error_ = "cannot bind NULL SQL parameter";
    return false;
  }
  return true;
}

bool AppStorage::statementBindInteger(uint32_t statement, uint32_t index,
                                      int64_t value) {
  StatementSession *session = findStatement(statement);
  if (!session || index == 0 || !session->statement.bindInt64(index, value)) {
    error_ = "cannot bind integer SQL parameter";
    return false;
  }
  return true;
}

bool AppStorage::statementBindFloat(uint32_t statement, uint32_t index,
                                    double value) {
  StatementSession *session = findStatement(statement);
  if (!session || index == 0 || !session->statement.bindDouble(index, value)) {
    error_ = "cannot bind float SQL parameter";
    return false;
  }
  return true;
}

bool AppStorage::statementBindText(uint32_t statement, uint32_t index,
                                   const std::string &value) {
  StatementSession *session = findStatement(statement);
  if (!session || index == 0 || value.size() > 4 * 1024 * 1024 ||
      !session->statement.bindText(index, value)) {
    error_ = "cannot bind text SQL parameter";
    return false;
  }
  return true;
}

bool AppStorage::statementBindBlob(uint32_t statement, uint32_t index,
                                   const uint8_t *value, size_t size) {
  StatementSession *session = findStatement(statement);
  if (!session || index == 0 || size > 4 * 1024 * 1024 ||
      (size > 0 && !value) ||
      !session->statement.bindBlob(index, value, size)) {
    error_ = "cannot bind blob SQL parameter";
    return false;
  }
  return true;
}

bool AppStorage::statementColumnCount(uint32_t statement, uint32_t &count) {
  StatementSession *session = findStatement(statement);
  if (!session)
    return false;
  count = static_cast<uint32_t>(session->statement.columnCount());
  return true;
}

bool AppStorage::statementColumnKind(uint32_t statement, uint32_t column,
                                     SqlValueKind &kind) {
  StatementSession *session = findStatement(statement);
  if (!session ||
      column >= static_cast<uint32_t>(session->statement.columnCount())) {
    error_ = "SQL column index is invalid";
    return false;
  }
  switch (session->statement.columnType(static_cast<int>(column))) {
  case 1:
    kind = SqlValueKind::Integer;
    break;
  case 2:
    kind = SqlValueKind::Float;
    break;
  case 3:
    kind = SqlValueKind::Text;
    break;
  case 4:
    kind = SqlValueKind::Blob;
    break;
  default:
    kind = SqlValueKind::Null;
    break;
  }
  return true;
}

bool AppStorage::statementColumnInt64(uint32_t statement, uint32_t column,
                                      int64_t &value) {
  SqlValueKind kind;
  StatementSession *session = findStatement(statement);
  if (!session || !statementColumnKind(statement, column, kind) ||
      kind != SqlValueKind::Integer) {
    error_ = "SQL column is not an integer";
    return false;
  }
  value = session->statement.columnInt64(static_cast<int>(column));
  return true;
}

bool AppStorage::statementColumnDouble(uint32_t statement, uint32_t column,
                                       double &value) {
  SqlValueKind kind;
  StatementSession *session = findStatement(statement);
  if (!session || !statementColumnKind(statement, column, kind) ||
      kind != SqlValueKind::Float) {
    error_ = "SQL column is not a float";
    return false;
  }
  value = session->statement.columnDouble(static_cast<int>(column));
  return true;
}

bool AppStorage::statementColumnText(uint32_t statement, uint32_t column,
                                     std::string &value) {
  SqlValueKind kind;
  StatementSession *session = findStatement(statement);
  if (!session || !statementColumnKind(statement, column, kind) ||
      kind != SqlValueKind::Text) {
    error_ = "SQL column is not text";
    return false;
  }
  const char *text = session->statement.columnText(static_cast<int>(column));
  const int size = session->statement.columnBytes(static_cast<int>(column));
  value.assign(size > 0 ? text : "", static_cast<size_t>(size));
  return true;
}

bool AppStorage::statementColumnBlob(uint32_t statement, uint32_t column,
                                     std::vector<uint8_t> &value) {
  SqlValueKind kind;
  StatementSession *session = findStatement(statement);
  if (!session || !statementColumnKind(statement, column, kind) ||
      kind != SqlValueKind::Blob) {
    error_ = "SQL column is not a blob";
    return false;
  }
  const uint8_t *blob = session->statement.columnBlob(static_cast<int>(column));
  const int size = session->statement.columnBytes(static_cast<int>(column));
  value.clear();
  if (size > 0)
    value.assign(blob, blob + size);
  return true;
}

bool AppStorage::statementFinish(uint32_t statement) {
  for (auto position = statements_.begin(); position != statements_.end();
       ++position) {
    if ((*position)->handle == statement) {
      statements_.erase(position);
      return true;
    }
  }
  error_ = "SQL statement handle is invalid";
  return false;
}

} // namespace oos::storage
