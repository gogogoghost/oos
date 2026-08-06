#include "app.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static _Atomic unsigned worker_result;
static _Atomic unsigned worker_failures;
static _Atomic bool worker_stop;
static pthread_t worker_thread;
static bool worker_started;

static void *worker(void *unused) {
  (void)unused;
  uint64_t previous = oos_platform_runtime_monotonic_time_us();
  while (!__c11_atomic_load(&worker_stop, __ATOMIC_ACQUIRE)) {
    const uint64_t current = oos_platform_runtime_monotonic_time_us();
    const int64_t wall = oos_platform_runtime_wall_clock_time_ms();
    if (current < previous || wall < 1577836800000LL)
      __c11_atomic_fetch_add(&worker_failures, 1, __ATOMIC_RELAXED);
    previous = current;
    __c11_atomic_fetch_add(&worker_result, 1, __ATOMIC_RELAXED);
    if ((__c11_atomic_load(&worker_result, __ATOMIC_RELAXED) & 1023) == 0)
      oos_platform_runtime_wake_main_thread();
  }
  return NULL;
}

bool exports_oos_platform_lifecycle_init(
    exports_oos_platform_lifecycle_error_code_t *error) {
  __c11_atomic_store(&worker_result, 0, __ATOMIC_RELAXED);
  __c11_atomic_store(&worker_failures, 0, __ATOMIC_RELAXED);
  __c11_atomic_store(&worker_stop, false, __ATOMIC_RELAXED);
  if (pthread_create(&worker_thread, NULL, worker, NULL) != 0) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  worker_started = true;
  pthread_t rejected_thread;
  if (pthread_create(&rejected_thread, NULL, worker, NULL) == 0) {
    __c11_atomic_store(&worker_stop, true, __ATOMIC_RELEASE);
    pthread_join(rejected_thread, NULL);
    pthread_join(worker_thread, NULL);
    worker_started = false;
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
  if (__c11_atomic_load(&worker_result, __ATOMIC_RELAXED) == 0 ||
      __c11_atomic_load(&worker_failures, __ATOMIC_RELAXED) != 0) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  *next_delay_ms = 1;
  return true;
}

void exports_oos_platform_lifecycle_shutdown(void) {
  if (!worker_started)
    return;
  __c11_atomic_store(&worker_stop, true, __ATOMIC_RELEASE);
  pthread_join(worker_thread, NULL);
  worker_started = false;
}
