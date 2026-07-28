#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oos::storage {

class SqliteStatement;

class SqliteDatabase {
public:
  SqliteDatabase();
  ~SqliteDatabase();
  SqliteDatabase(SqliteDatabase &&) noexcept;
  SqliteDatabase &operator=(SqliteDatabase &&) noexcept;

  SqliteDatabase(const SqliteDatabase &) = delete;
  SqliteDatabase &operator=(const SqliteDatabase &) = delete;

  bool open(const std::string &path);
  void close();
  bool exec(const char *sql);
  bool prepare(const char *sql, SqliteStatement &statement);
  int64_t lastInsertRowId() const;
  int changes() const;
  const std::string &lastError() const;

private:
  friend class SqliteStatement;
  class Impl;
  std::unique_ptr<Impl> impl_;
};

class SqliteStatement {
public:
  enum class Step { Row, Done, Error };

  SqliteStatement();
  ~SqliteStatement();
  SqliteStatement(SqliteStatement &&) noexcept;
  SqliteStatement &operator=(SqliteStatement &&) noexcept;

  SqliteStatement(const SqliteStatement &) = delete;
  SqliteStatement &operator=(const SqliteStatement &) = delete;

  bool bindText(int index, const std::string &value);
  bool bindInt64(int index, int64_t value);
  bool bindDouble(int index, double value);
  bool bindBlob(int index, const uint8_t *data, size_t size);
  bool bindNull(int index);
  Step step();
  bool reset();
  const char *columnText(int index) const;
  int64_t columnInt64(int index) const;
  const uint8_t *columnBlob(int index) const;
  int columnBytes(int index) const;
  int columnCount() const;
  int columnType(int index) const;
  double columnDouble(int index) const;

private:
  friend class SqliteDatabase;
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace oos::storage
