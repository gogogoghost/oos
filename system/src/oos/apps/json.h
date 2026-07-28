#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace oos::apps {

class JsonParser;

class JsonValue {
public:
  enum class Type { Null, Boolean, Number, String, Array, Object };

  Type type() const { return type_; }
  bool isObject() const { return type_ == Type::Object; }
  bool isArray() const { return type_ == Type::Array; }
  bool isString() const { return type_ == Type::String; }
  bool isNumber() const { return type_ == Type::Number; }
  bool isBoolean() const { return type_ == Type::Boolean; }

  const JsonValue *get(const char *key) const;
  const std::string &stringValue() const { return string_; }
  int64_t integerValue() const { return integer_; }
  bool booleanValue() const { return boolean_; }
  const std::vector<JsonValue> &arrayValue() const { return array_; }
  const std::map<std::string, JsonValue> &objectValue() const {
    return object_;
  }

private:
  friend class JsonParser;

  Type type_ = Type::Null;
  bool boolean_ = false;
  int64_t integer_ = 0;
  std::string string_;
  std::vector<JsonValue> array_;
  std::map<std::string, JsonValue> object_;
};

bool parseJson(const std::string &input, JsonValue &output, std::string &error);

} // namespace oos::apps
