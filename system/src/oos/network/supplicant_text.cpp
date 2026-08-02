#include "oos/network/supplicant_text.h"

namespace oos::network {
namespace {

int hexValue(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}

} // namespace

std::string decodeSupplicantText(std::string_view value) {
  std::string decoded;
  decoded.reserve(value.size());
  for (size_t index = 0; index < value.size(); ++index) {
    const char current = value[index];
    if (current != '\\' || index + 1 >= value.size()) {
      decoded.push_back(current);
      continue;
    }

    const char escaped = value[index + 1];
    if (escaped == 'x' && index + 3 < value.size()) {
      const int high = hexValue(value[index + 2]);
      const int low = hexValue(value[index + 3]);
      if (high >= 0 && low >= 0) {
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 3;
        continue;
      }
    }

    char replacement = 0;
    switch (escaped) {
    case '\\':
    case '"':
      replacement = escaped;
      break;
    case 'n':
      replacement = '\n';
      break;
    case 'r':
      replacement = '\r';
      break;
    case 't':
      replacement = '\t';
      break;
    case 'e':
      replacement = '\x1b';
      break;
    default:
      decoded.push_back(current);
      continue;
    }
    decoded.push_back(replacement);
    ++index;
  }
  return decoded;
}

} // namespace oos::network
