#include "app.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void log_error(const char *text) {
  app_string_t message;
  app_string_set(&message, text);
  oos_platform_runtime_log(OOS_PLATFORM_RUNTIME_LOG_LEVEL_ERROR, &message);
}

bool exports_oos_platform_lifecycle_init(
    exports_oos_platform_lifecycle_error_code_t *error) {
  oos_platform_modules_list_module_info_t modules = {0};
  oos_platform_modules_enumerate(&modules);
  bool wasm_declared = false;
  bool js_declared = false;
  for (size_t index = 0; index < modules.len; ++index) {
    if (modules.ptr[index].name.len == 4 &&
        memcmp(modules.ptr[index].name.ptr, "echo", 4) == 0 &&
        modules.ptr[index].kind == OOS_PLATFORM_MODULES_MODULE_KIND_WASM) {
      wasm_declared = true;
    } else if (modules.ptr[index].name.len == 7 &&
               memcmp(modules.ptr[index].name.ptr, "js-echo", 7) == 0 &&
               modules.ptr[index].kind ==
                   OOS_PLATFORM_MODULES_MODULE_KIND_JS) {
      js_declared = true;
    }
  }
  oos_platform_modules_list_module_info_free(&modules);
  if (!wasm_declared || !js_declared) {
    log_error("package module declarations were not enumerated");
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }

  app_string_t operation;
  app_string_set(&operation, "echo");
  uint8_t input[] = {1, 3, 5, 7, 9};
  const char *module_names[] = {"echo", "js-echo"};
  for (size_t index = 0; index < 2; ++index) {
    app_string_t module;
    app_string_set(&module, module_names[index]);
    app_list_u8_t request = {input, sizeof(input)};
    app_list_u8_t response = {0};
    oos_platform_modules_error_code_t invoke_error;
    const bool success = oos_platform_modules_invoke(
        &module, &operation, &request, &response, &invoke_error);
    const bool valid = success && response.len == sizeof(input) &&
                       memcmp(response.ptr, input, sizeof(input)) == 0;
    app_list_u8_free(&response);
    if (!valid) {
      log_error("package module echo invocation failed");
      *error = success ? OOS_PLATFORM_TYPES_ERROR_CODE_FAILED : invoke_error;
      return false;
    }
  }
  return true;
}

void exports_oos_platform_lifecycle_event(
    exports_oos_platform_lifecycle_key_event_t *event) {
  (void)event;
}

bool exports_oos_platform_lifecycle_frame(
    uint64_t monotonic_time_us, uint32_t *next_delay_ms,
    exports_oos_platform_lifecycle_error_code_t *error) {
  (void)monotonic_time_us;
  (void)error;
  *next_delay_ms = 1000;
  return true;
}

void exports_oos_platform_lifecycle_shutdown(void) {}
