#include "oos/network/supplicant_text.h"

#include <cassert>
#include <string>

int main() {
  using oos::network::decodeSupplicantText;

  assert(decodeSupplicantText("Orange") == "Orange");
  assert(decodeSupplicantText("\\xe5\\xa4\\xa7\\xe5\\xae\\xb6") ==
         "\xe5\xa4\xa7\xe5\xae\xb6");
  assert(decodeSupplicantText("Cafe\\x20Wi-Fi") == "Cafe Wi-Fi");
  assert(decodeSupplicantText("quote\\\" slash\\\\") == "quote\" slash\\");
  assert(decodeSupplicantText("line\\n tab\\t escape\\e") ==
         std::string("line\n tab\t escape\x1b"));
  assert(decodeSupplicantText("unknown\\q malformed\\xG1 trailing\\") ==
         "unknown\\q malformed\\xG1 trailing\\");
  return 0;
}
