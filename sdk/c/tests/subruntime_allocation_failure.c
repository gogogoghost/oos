#include "app.h"

#include <stdlib.h>

bool exports_oos_platform_lifecycle_init(
    exports_oos_platform_lifecycle_error_code_t *error) {
  void *allocations[3] = {0};
  for (unsigned index = 0; index < 3; ++index) {
    allocations[index] = malloc(7 * 1024 * 1024);
    if (!allocations[index]) {
      for (unsigned prior = 0; prior < index; ++prior)
        free(allocations[prior]);
      *error = OOS_PLATFORM_TYPES_ERROR_CODE_LIMIT_EXCEEDED;
      return false;
    }
    ((volatile unsigned char *)allocations[index])[index * 4096] =
        (unsigned char)index;
  }
  for (unsigned index = 0; index < 3; ++index)
    free(allocations[index]);
  if (allocations[2])
    return true;
  *error = OOS_PLATFORM_TYPES_ERROR_CODE_LIMIT_EXCEEDED;
  return false;
}

void exports_oos_platform_lifecycle_event(
    exports_oos_platform_lifecycle_key_event_t *event) {
  (void)event;
}

bool exports_oos_platform_lifecycle_frame(
    uint64_t monotonic_time_us, uint32_t *next_delay_ms,
    exports_oos_platform_lifecycle_error_code_t *error) {
  (void)monotonic_time_us;
  (void)next_delay_ms;
  *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
  return false;
}

void exports_oos_platform_lifecycle_shutdown(void) {}
