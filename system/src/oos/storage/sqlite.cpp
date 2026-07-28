#include "oos/storage/sqlite.h"

#include <dlfcn.h>

#include <cstring>
#include <utility>

namespace oos::storage {
namespace {

struct sqlite3;
struct sqlite3_stmt;
using SqliteDestructor = void (*)(void *);

constexpr int kOk = 0;
constexpr int kRow = 100;
constexpr int kDone = 101;
constexpr int kOpenReadWrite = 0x00000002;
constexpr int kOpenCreate = 0x00000004;
constexpr int kOpenFullMutex = 0x00010000;
const SqliteDestructor kTransient =
    reinterpret_cast<SqliteDestructor>(static_cast<intptr_t>(-1));

class SqliteApi {
public:
  using OpenV2 = int (*)(const char *, sqlite3 **, int, const char *);
  using CloseV2 = int (*)(sqlite3 *);
  using ErrorMessage = const char *(*)(sqlite3 *);
  using Exec = int (*)(sqlite3 *, const char *,
                       int (*)(void *, int, char **, char **), void *, char **);
  using Free = void (*)(void *);
  using PrepareV2 = int (*)(sqlite3 *, const char *, int, sqlite3_stmt **,
                            const char **);
  using Finalize = int (*)(sqlite3_stmt *);
  using BindText = int (*)(sqlite3_stmt *, int, const char *, int,
                           SqliteDestructor);
  using BindInt64 = int (*)(sqlite3_stmt *, int, int64_t);
  using BindDouble = int (*)(sqlite3_stmt *, int, double);
  using BindBlob = int (*)(sqlite3_stmt *, int, const void *, int,
                           SqliteDestructor);
  using BindNull = int (*)(sqlite3_stmt *, int);
  using Step = int (*)(sqlite3_stmt *);
  using Reset = int (*)(sqlite3_stmt *);
  using ClearBindings = int (*)(sqlite3_stmt *);
  using ColumnText = const unsigned char *(*)(sqlite3_stmt *, int);
  using ColumnInt64 = int64_t (*)(sqlite3_stmt *, int);
  using ColumnBlob = const void *(*)(sqlite3_stmt *, int);
  using ColumnBytes = int (*)(sqlite3_stmt *, int);
  using ColumnCount = int (*)(sqlite3_stmt *);
  using ColumnType = int (*)(sqlite3_stmt *, int);
  using ColumnDouble = double (*)(sqlite3_stmt *, int);
  using BusyTimeout = int (*)(sqlite3 *, int);
  using LastInsertRowId = int64_t (*)(sqlite3 *);
  using Changes = int (*)(sqlite3 *);

  static SqliteApi &instance() {
    static SqliteApi api;
    return api;
  }

  bool available() const { return handle_ != nullptr; }
  const std::string &error() const { return error_; }

  OpenV2 open_v2 = nullptr;
  CloseV2 close_v2 = nullptr;
  ErrorMessage errmsg = nullptr;
  Exec exec = nullptr;
  Free free_memory = nullptr;
  PrepareV2 prepare_v2 = nullptr;
  Finalize finalize = nullptr;
  BindText bind_text = nullptr;
  BindInt64 bind_int64 = nullptr;
  BindDouble bind_double = nullptr;
  BindBlob bind_blob = nullptr;
  BindNull bind_null = nullptr;
  Step step = nullptr;
  Reset reset = nullptr;
  ClearBindings clear_bindings = nullptr;
  ColumnText column_text = nullptr;
  ColumnInt64 column_int64 = nullptr;
  ColumnBlob column_blob = nullptr;
  ColumnBytes column_bytes = nullptr;
  ColumnCount column_count = nullptr;
  ColumnType column_type = nullptr;
  ColumnDouble column_double = nullptr;
  BusyTimeout busy_timeout = nullptr;
  LastInsertRowId last_insert_rowid = nullptr;
  Changes changes = nullptr;

private:
  SqliteApi() {
    handle_ = dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle_)
      handle_ = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!handle_)
      handle_ = dlopen("libsqlite.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
      const char *message = dlerror();
      error_ = message ? message : "libsqlite3 is unavailable";
      return;
    }
#define OOS_SQLITE_SYMBOL(member, symbol)                                      \
  do {                                                                         \
    member = reinterpret_cast<decltype(member)>(dlsym(handle_, symbol));       \
    if (!member) {                                                             \
      error_ = std::string("missing SQLite symbol: ") + symbol;                \
      dlclose(handle_);                                                        \
      handle_ = nullptr;                                                       \
      return;                                                                  \
    }                                                                          \
  } while (false)
    OOS_SQLITE_SYMBOL(open_v2, "sqlite3_open_v2");
    OOS_SQLITE_SYMBOL(close_v2, "sqlite3_close_v2");
    OOS_SQLITE_SYMBOL(errmsg, "sqlite3_errmsg");
    OOS_SQLITE_SYMBOL(exec, "sqlite3_exec");
    OOS_SQLITE_SYMBOL(free_memory, "sqlite3_free");
    OOS_SQLITE_SYMBOL(prepare_v2, "sqlite3_prepare_v2");
    OOS_SQLITE_SYMBOL(finalize, "sqlite3_finalize");
    OOS_SQLITE_SYMBOL(bind_text, "sqlite3_bind_text");
    OOS_SQLITE_SYMBOL(bind_int64, "sqlite3_bind_int64");
    OOS_SQLITE_SYMBOL(bind_double, "sqlite3_bind_double");
    OOS_SQLITE_SYMBOL(bind_blob, "sqlite3_bind_blob");
    OOS_SQLITE_SYMBOL(bind_null, "sqlite3_bind_null");
    OOS_SQLITE_SYMBOL(step, "sqlite3_step");
    OOS_SQLITE_SYMBOL(reset, "sqlite3_reset");
    OOS_SQLITE_SYMBOL(clear_bindings, "sqlite3_clear_bindings");
    OOS_SQLITE_SYMBOL(column_text, "sqlite3_column_text");
    OOS_SQLITE_SYMBOL(column_int64, "sqlite3_column_int64");
    OOS_SQLITE_SYMBOL(column_blob, "sqlite3_column_blob");
    OOS_SQLITE_SYMBOL(column_bytes, "sqlite3_column_bytes");
    OOS_SQLITE_SYMBOL(column_count, "sqlite3_column_count");
    OOS_SQLITE_SYMBOL(column_type, "sqlite3_column_type");
    OOS_SQLITE_SYMBOL(column_double, "sqlite3_column_double");
    OOS_SQLITE_SYMBOL(busy_timeout, "sqlite3_busy_timeout");
    OOS_SQLITE_SYMBOL(last_insert_rowid, "sqlite3_last_insert_rowid");
    OOS_SQLITE_SYMBOL(changes, "sqlite3_changes");
#undef OOS_SQLITE_SYMBOL
  }

  void *handle_ = nullptr;
  std::string error_;
};

} // namespace

class SqliteDatabase::Impl {
public:
  void setDatabaseError(const char *prefix) {
    const char *message = database ? SqliteApi::instance().errmsg(database)
                                   : "database is closed";
    error = std::string(prefix) + ": " + (message ? message : "unknown error");
  }

  sqlite3 *database = nullptr;
  std::string error;
};

class SqliteStatement::Impl {
public:
  sqlite3_stmt *statement = nullptr;
  SqliteDatabase::Impl *database = nullptr;
};

SqliteDatabase::SqliteDatabase() : impl_(std::make_unique<Impl>()) {}
SqliteDatabase::~SqliteDatabase() { close(); }
SqliteDatabase::SqliteDatabase(SqliteDatabase &&) noexcept = default;
SqliteDatabase &SqliteDatabase::operator=(SqliteDatabase &&) noexcept = default;

bool SqliteDatabase::open(const std::string &path) {
  close();
  SqliteApi &api = SqliteApi::instance();
  if (!api.available()) {
    impl_->error = api.error();
    return false;
  }
  if (api.open_v2(path.c_str(), &impl_->database,
                  kOpenReadWrite | kOpenCreate | kOpenFullMutex,
                  nullptr) != kOk) {
    impl_->setDatabaseError("open SQLite database");
    close();
    return false;
  }
  api.busy_timeout(impl_->database, 5000);
  if (!exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; "
            "PRAGMA foreign_keys=ON; PRAGMA temp_store=MEMORY;")) {
    close();
    return false;
  }
  return true;
}

void SqliteDatabase::close() {
  if (impl_ && impl_->database) {
    SqliteApi::instance().close_v2(impl_->database);
    impl_->database = nullptr;
  }
}

bool SqliteDatabase::exec(const char *sql) {
  if (!impl_->database) {
    impl_->error = "SQLite database is closed";
    return false;
  }
  char *message = nullptr;
  const int result = SqliteApi::instance().exec(impl_->database, sql, nullptr,
                                                nullptr, &message);
  if (result == kOk)
    return true;
  impl_->error = message ? message : "SQLite statement failed";
  if (message)
    SqliteApi::instance().free_memory(message);
  return false;
}

bool SqliteDatabase::prepare(const char *sql, SqliteStatement &statement) {
  statement = SqliteStatement();
  if (!impl_->database) {
    impl_->error = "SQLite database is closed";
    return false;
  }
  if (SqliteApi::instance().prepare_v2(impl_->database, sql, -1,
                                       &statement.impl_->statement,
                                       nullptr) != kOk) {
    impl_->setDatabaseError("prepare SQLite statement");
    return false;
  }
  statement.impl_->database = impl_.get();
  return true;
}

int64_t SqliteDatabase::lastInsertRowId() const {
  return impl_->database
             ? SqliteApi::instance().last_insert_rowid(impl_->database)
             : 0;
}

int SqliteDatabase::changes() const {
  return impl_->database ? SqliteApi::instance().changes(impl_->database) : 0;
}

const std::string &SqliteDatabase::lastError() const { return impl_->error; }

SqliteStatement::SqliteStatement() : impl_(std::make_unique<Impl>()) {}
SqliteStatement::~SqliteStatement() {
  if (impl_ && impl_->statement)
    SqliteApi::instance().finalize(impl_->statement);
}
SqliteStatement::SqliteStatement(SqliteStatement &&) noexcept = default;
SqliteStatement &SqliteStatement::operator=(SqliteStatement &&other) noexcept {
  if (this == &other)
    return *this;
  if (impl_ && impl_->statement)
    SqliteApi::instance().finalize(impl_->statement);
  impl_ = std::move(other.impl_);
  return *this;
}

bool SqliteStatement::bindText(int index, const std::string &value) {
  return impl_->statement &&
         SqliteApi::instance().bind_text(impl_->statement, index, value.data(),
                                         static_cast<int>(value.size()),
                                         kTransient) == kOk;
}

bool SqliteStatement::bindInt64(int index, int64_t value) {
  return impl_->statement && SqliteApi::instance().bind_int64(
                                 impl_->statement, index, value) == kOk;
}

bool SqliteStatement::bindDouble(int index, double value) {
  return impl_->statement && SqliteApi::instance().bind_double(
                                 impl_->statement, index, value) == kOk;
}

bool SqliteStatement::bindBlob(int index, const uint8_t *data, size_t size) {
  static const uint8_t empty = 0;
  return impl_->statement && size <= static_cast<size_t>(INT32_MAX) &&
         SqliteApi::instance().bind_blob(
             impl_->statement, index, data ? data : &empty,
             static_cast<int>(size), kTransient) == kOk;
}

bool SqliteStatement::bindNull(int index) {
  return impl_->statement &&
         SqliteApi::instance().bind_null(impl_->statement, index) == kOk;
}

SqliteStatement::Step SqliteStatement::step() {
  if (!impl_->statement)
    return Step::Error;
  const int result = SqliteApi::instance().step(impl_->statement);
  if (result == kRow)
    return Step::Row;
  if (result == kDone)
    return Step::Done;
  if (impl_->database)
    impl_->database->setDatabaseError("execute SQLite statement");
  return Step::Error;
}

bool SqliteStatement::reset() {
  return impl_->statement &&
         SqliteApi::instance().reset(impl_->statement) == kOk &&
         SqliteApi::instance().clear_bindings(impl_->statement) == kOk;
}

const char *SqliteStatement::columnText(int index) const {
  return reinterpret_cast<const char *>(
      SqliteApi::instance().column_text(impl_->statement, index));
}

int64_t SqliteStatement::columnInt64(int index) const {
  return SqliteApi::instance().column_int64(impl_->statement, index);
}

const uint8_t *SqliteStatement::columnBlob(int index) const {
  return static_cast<const uint8_t *>(
      SqliteApi::instance().column_blob(impl_->statement, index));
}

int SqliteStatement::columnBytes(int index) const {
  return SqliteApi::instance().column_bytes(impl_->statement, index);
}

int SqliteStatement::columnCount() const {
  return SqliteApi::instance().column_count(impl_->statement);
}

int SqliteStatement::columnType(int index) const {
  return SqliteApi::instance().column_type(impl_->statement, index);
}

double SqliteStatement::columnDouble(int index) const {
  return SqliteApi::instance().column_double(impl_->statement, index);
}

} // namespace oos::storage
