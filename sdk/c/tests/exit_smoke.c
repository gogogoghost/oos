#include "app.h"

#include <stdbool.h>

bool exports_oos_platform_lifecycle_init(
    exports_oos_platform_lifecycle_error_code_t *error) {
  oos_platform_runtime_error_code_t runtime_error;
  if (!oos_platform_runtime_request_exit(&runtime_error)) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  return true;
}

void exports_oos_platform_lifecycle_event(
    exports_oos_platform_lifecycle_key_event_t *event) {
  (void)event;
}

bool exports_oos_platform_lifecycle_frame(
    uint64_t monotonic_time_us,
    exports_oos_platform_lifecycle_error_code_t *error) {
  (void)monotonic_time_us;
  (void)error;
  return true;
}

void exports_oos_platform_lifecycle_shutdown(void) {}
