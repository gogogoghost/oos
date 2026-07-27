#pragma once

#include <string>

namespace oos::device::nokia8110 {

// B2G owns Android's permission Binder service on the stock system. OOS must
// provide it after B2G exits so native framework services do not block.
bool ensurePermissionController(std::string &error);

} // namespace oos::device::nokia8110
