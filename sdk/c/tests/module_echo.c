#include "module.h"

#include <stdlib.h>
#include <string.h>

bool exports_oos_platform_module_api_invoke(
    module_string_t *operation, module_list_u8_t *request,
    module_list_u8_t *response,
    exports_oos_platform_module_api_error_code_t *error) {
  static const char expected[] = "echo";
  if (operation->len != sizeof(expected) - 1 ||
      memcmp(operation->ptr, expected, sizeof(expected) - 1) != 0) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_INVALID_ARGUMENT;
    return false;
  }
  response->ptr = request->len ? malloc(request->len) : NULL;
  response->len = request->len;
  if (request->len && !response->ptr) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_LIMIT_EXCEEDED;
    return false;
  }
  if (request->len)
    memcpy(response->ptr, request->ptr, request->len);
  return true;
}
