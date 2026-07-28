#pragma once

#include "oos/storage/sqlite.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oos::storage {

enum class SqlRowState : uint8_t { Row, Done };
enum class SqlValueKind : uint8_t { Integer, Float, Text, Blob, Null };

class AppStorage {
public:
  explicit AppStorage(std::string app_data_directory);
  ~AppStorage();

  bool initialize();
  bool get(const std::string &key, std::vector<uint8_t> &value, bool &found);
  bool set(const std::string &key, const uint8_t *value, size_t size);
  bool remove(const std::string &key, bool &removed);
  bool clear();
  bool openDatabase(const std::string &name,
                    std::unique_ptr<SqliteDatabase> &database);
  bool databaseExecute(const std::string &name, const std::string &sql,
                       uint32_t &changes);
  bool databasePrepare(const std::string &name, const std::string &sql,
                       uint32_t &statement);
  bool statementStep(uint32_t statement, SqlRowState &state);
  bool statementBindNull(uint32_t statement, uint32_t index);
  bool statementBindInteger(uint32_t statement, uint32_t index, int64_t value);
  bool statementBindFloat(uint32_t statement, uint32_t index, double value);
  bool statementBindText(uint32_t statement, uint32_t index,
                         const std::string &value);
  bool statementBindBlob(uint32_t statement, uint32_t index,
                         const uint8_t *value, size_t size);
  bool statementColumnCount(uint32_t statement, uint32_t &count);
  bool statementColumnKind(uint32_t statement, uint32_t column,
                           SqlValueKind &kind);
  bool statementColumnInt64(uint32_t statement, uint32_t column,
                            int64_t &value);
  bool statementColumnDouble(uint32_t statement, uint32_t column,
                             double &value);
  bool statementColumnText(uint32_t statement, uint32_t column,
                           std::string &value);
  bool statementColumnBlob(uint32_t statement, uint32_t column,
                           std::vector<uint8_t> &value);
  bool statementFinish(uint32_t statement);

  const std::string &dataDirectory() const { return data_directory_; }
  const std::string &lastError() const { return error_; }

private:
  struct StatementSession;
  bool validKey(const std::string &key) const;
  bool validDatabaseName(const std::string &name);
  StatementSession *findStatement(uint32_t statement);

  std::string data_directory_;
  SqliteDatabase kv_;
  std::vector<std::unique_ptr<StatementSession>> statements_;
  uint32_t next_statement_ = 1;
  std::string error_;
};

} // namespace oos::storage
