#include "app.h"

#include <pthread.h>
#include <stdbool.h>

static void *worker(void *unused) {
  (void)unused;
  (void)oos_platform_graphics_surface_format();
  return NULL;
}

bool exports_oos_platform_lifecycle_init(
    exports_oos_platform_lifecycle_error_code_t *error) {
  pthread_t thread;
  if (pthread_create(&thread, NULL, worker, NULL) != 0 ||
      pthread_join(thread, NULL) != 0) {
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
    uint64_t monotonic_time_us, uint32_t *next_delay_ms,
    exports_oos_platform_lifecycle_error_code_t *error) {
  (void)monotonic_time_us;
  (void)error;
  *next_delay_ms = 1000;
  return true;
}

void exports_oos_platform_lifecycle_shutdown(void) {}
