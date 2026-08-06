#include "app.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

static __attribute__((noinline)) uint32_t consume_stack(uint32_t depth) {
  const size_t frame_size =
      1024 + (size_t)(oos_platform_runtime_monotonic_time_us() & 31);
  volatile uint8_t *frame = __builtin_alloca(frame_size);
  frame[depth % frame_size] = (uint8_t)depth;
  const uint32_t nested = consume_stack(depth + 1);
  return frame[(depth + 1) % frame_size] + nested;
}

static void *overflow_worker(void *unused) {
  (void)unused;
  return (void *)(uintptr_t)consume_stack(0);
}

bool exports_oos_platform_lifecycle_init(
    exports_oos_platform_lifecycle_error_code_t *error) {
  pthread_t worker;
  if (pthread_create(&worker, NULL, overflow_worker, NULL) != 0 ||
      pthread_join(worker, NULL) != 0) {
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
  (void)next_delay_ms;
  *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
  return false;
}

void exports_oos_platform_lifecycle_shutdown(void) {}
