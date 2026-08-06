#include "app.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_int(const void *left, const void *right) {
  const int a = *(const int *)left;
  const int b = *(const int *)right;
  return (a > b) - (a < b);
}

bool exports_oos_platform_lifecycle_init(
    exports_oos_platform_lifecycle_error_code_t *error) {
  int values[] = {4, 1, 3, 2};
  qsort(values, 4, sizeof(values[0]), compare_int);
  char *buffer = malloc(64);
  if (!buffer) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  const double value = sin(0.5) * sin(0.5) + cos(0.5) * cos(0.5);
  const int length =
      snprintf(buffer, 64, "%c:%d:%.3f", toupper('o'), values[3], value);
  const bool valid = length == 9 && strcmp(buffer, "O:4:1.000") == 0;
  free(buffer);
  if (!valid)
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
  return valid;
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
