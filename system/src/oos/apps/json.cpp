#include "oos/apps/json.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace oos::apps {

class JsonParser {
public:
  explicit JsonParser(const std::string &input) : input_(input) {}

  bool parse(JsonValue &output) {
    skipSpace();
    if (!parseValue(output))
      return false;
    skipSpace();
    if (position_ != input_.size())
      return fail("trailing data");
    return true;
  }

  const std::string &error() const { return error_; }

private:
  bool parseValue(JsonValue &value) {
    if (position_ >= input_.size())
      return fail("unexpected end of input");
    switch (input_[position_]) {
    case '{':
      return parseObject(value);
    case '[':
      return parseArray(value);
    case '"':
      value.type_ = JsonValue::Type::String;
      return parseString(value.string_);
    case 't':
      value.type_ = JsonValue::Type::Boolean;
      value.boolean_ = true;
      return consumeLiteral("true");
    case 'f':
      value.type_ = JsonValue::Type::Boolean;
      value.boolean_ = false;
      return consumeLiteral("false");
    case 'n':
      value.type_ = JsonValue::Type::Null;
      return consumeLiteral("null");
    default:
      if (input_[position_] == '-' || std::isdigit(input_[position_]))
        return parseInteger(value);
      return fail("unexpected token");
    }
  }

  bool parseObject(JsonValue &value) {
    ++position_;
    value.type_ = JsonValue::Type::Object;
    value.object_.clear();
    skipSpace();
    if (consume('}'))
      return true;
    while (true) {
      std::string key;
      if (!parseString(key))
        return false;
      skipSpace();
      if (!consume(':'))
        return fail("expected ':'");
      skipSpace();
      JsonValue child;
      if (!parseValue(child))
        return false;
      if (!value.object_.emplace(std::move(key), std::move(child)).second)
        return fail("duplicate object key");
      skipSpace();
      if (consume('}'))
        return true;
      if (!consume(','))
        return fail("expected ',' or '}'");
      skipSpace();
    }
  }

  bool parseArray(JsonValue &value) {
    ++position_;
    value.type_ = JsonValue::Type::Array;
    value.array_.clear();
    skipSpace();
    if (consume(']'))
      return true;
    while (true) {
      JsonValue child;
      if (!parseValue(child))
        return false;
      value.array_.push_back(std::move(child));
      skipSpace();
      if (consume(']'))
        return true;
      if (!consume(','))
        return fail("expected ',' or ']'");
      skipSpace();
    }
  }

  bool parseString(std::string &output) {
    if (!consume('"'))
      return fail("expected string");
    output.clear();
    while (position_ < input_.size()) {
      const unsigned char character = input_[position_++];
      if (character == '"')
        return true;
      if (character < 0x20)
        return fail("control character in string");
      if (character != '\\') {
        output.push_back(static_cast<char>(character));
        continue;
      }
      if (position_ >= input_.size())
        return fail("unfinished string escape");
      switch (input_[position_++]) {
      case '"':
        output.push_back('"');
        break;
      case '\\':
        output.push_back('\\');
        break;
      case '/':
        output.push_back('/');
        break;
      case 'b':
        output.push_back('\b');
        break;
      case 'f':
        output.push_back('\f');
        break;
      case 'n':
        output.push_back('\n');
        break;
      case 'r':
        output.push_back('\r');
        break;
      case 't':
        output.push_back('\t');
        break;
      case 'u':
        if (!parseUnicode(output))
          return false;
        break;
      default:
        return fail("invalid string escape");
      }
    }
    return fail("unterminated string");
  }

  bool parseUnicode(std::string &output) {
    uint32_t codepoint = 0;
    for (int index = 0; index < 4; ++index) {
      if (position_ >= input_.size())
        return fail("unfinished unicode escape");
      const char character = input_[position_++];
      codepoint <<= 4;
      if (character >= '0' && character <= '9')
        codepoint += character - '0';
      else if (character >= 'a' && character <= 'f')
        codepoint += character - 'a' + 10;
      else if (character >= 'A' && character <= 'F')
        codepoint += character - 'A' + 10;
      else
        return fail("invalid unicode escape");
    }
    if (codepoint >= 0xd800 && codepoint <= 0xdfff)
      return fail("unicode surrogate escapes are not supported");
    if (codepoint <= 0x7f) {
      output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
      output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
      output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
      output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
      output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
    return true;
  }

  bool parseInteger(JsonValue &value) {
    const size_t start = position_;
    bool negative = consume('-');
    if (position_ >= input_.size() || !std::isdigit(input_[position_]))
      return fail("invalid number");
    if (input_[position_] == '0') {
      ++position_;
      if (position_ < input_.size() && std::isdigit(input_[position_]))
        return fail("leading zero in number");
    } else {
      while (position_ < input_.size() && std::isdigit(input_[position_]))
        ++position_;
    }
    if (position_ < input_.size() &&
        (input_[position_] == '.' || input_[position_] == 'e' ||
         input_[position_] == 'E'))
      return fail("manifest numbers must be integers");

    uint64_t magnitude = 0;
    const size_t digits = start + (negative ? 1 : 0);
    for (size_t index = digits; index < position_; ++index) {
      const uint32_t digit = input_[index] - '0';
      if (magnitude > (std::numeric_limits<uint64_t>::max() - digit) / 10)
        return fail("number is out of range");
      magnitude = magnitude * 10 + digit;
    }
    const uint64_t max_positive =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if ((!negative && magnitude > max_positive) ||
        (negative && magnitude > max_positive + 1))
      return fail("number is out of range");
    value.type_ = JsonValue::Type::Number;
    value.integer_ = negative ? (magnitude == max_positive + 1
                                     ? std::numeric_limits<int64_t>::min()
                                     : -static_cast<int64_t>(magnitude))
                              : static_cast<int64_t>(magnitude);
    return true;
  }

  bool consumeLiteral(const char *literal) {
    for (size_t index = 0; literal[index]; ++index) {
      if (position_ >= input_.size() || input_[position_++] != literal[index])
        return fail("invalid literal");
    }
    return true;
  }

  bool consume(char character) {
    if (position_ < input_.size() && input_[position_] == character) {
      ++position_;
      return true;
    }
    return false;
  }

  void skipSpace() {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\t' ||
            input_[position_] == '\r' || input_[position_] == '\n'))
      ++position_;
  }

  bool fail(const char *message) {
    if (error_.empty()) {
      char position[32] = {};
      std::snprintf(position, sizeof(position), "%lu",
                    static_cast<unsigned long>(position_));
      error_ = std::string(message) + " at byte " + position;
    }
    return false;
  }

  const std::string &input_;
  size_t position_ = 0;
  std::string error_;
};

const JsonValue *JsonValue::get(const char *key) const {
  if (!key || type_ != Type::Object)
    return nullptr;
  const auto found = object_.find(key);
  return found == object_.end() ? nullptr : &found->second;
}

bool parseJson(const std::string &input, JsonValue &output,
               std::string &error) {
  JsonParser parser(input);
  if (!parser.parse(output)) {
    error = parser.error();
    return false;
  }
  error.clear();
  return true;
}

} // namespace oos::apps
