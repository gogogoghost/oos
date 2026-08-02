#pragma once

#include <string>
#include <string_view>

namespace oos::network {

std::string decodeSupplicantText(std::string_view value);

} // namespace oos::network
