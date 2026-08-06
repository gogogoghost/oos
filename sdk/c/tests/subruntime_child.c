#include "app.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

static uint8_t *working_set;
static pthread_t worker_thread;
static atomic_uint worker_ready;
static atomic_uint worker_stop;
static int worker_started;
static unsigned frame_count;

static void log_error(const char *text) {
  app_string_t message;
  app_string_set(&message, text);
  oos_platform_runtime_log(OOS_PLATFORM_RUNTIME_LOG_LEVEL_ERROR, &message);
}

static void *worker(void *unused) {
  (void)unused;
  volatile uint8_t stack_probe[1536 * 1024];
  for (size_t offset = 0; offset < sizeof(stack_probe); offset += 4096)
    stack_probe[offset] = (uint8_t)(offset >> 12);
  for (size_t offset = 0; offset < 12 * 1024 * 1024; offset += 4096)
    working_set[offset] = (uint8_t)(offset >> 12);
  working_set[0] ^= stack_probe[0];
  atomic_store_explicit(&worker_ready, 1, memory_order_release);
  __builtin_wasm_memory_atomic_notify((int *)&worker_ready, 1);
  while (!atomic_load_explicit(&worker_stop, memory_order_acquire))
    __builtin_wasm_memory_atomic_wait32((int *)&worker_stop, 0, -1);
  return NULL;
}

bool exports_oos_platform_lifecycle_init(
    exports_oos_platform_lifecycle_error_code_t *error) {
  oos_platform_graphics_size_t surface;
  oos_platform_graphics_surface_size(&surface);
  if (surface.width == 0 || surface.height == 0) {
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  working_set = malloc(12 * 1024 * 1024);
  if (!working_set) {
    log_error("subruntime child allocation failed");
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  if (pthread_create(&worker_thread, NULL, worker, NULL) != 0) {
    log_error("subruntime child pthread_create failed");
    *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
    return false;
  }
  worker_started = 1;
  while (!atomic_load_explicit(&worker_ready, memory_order_acquire))
    __builtin_wasm_memory_atomic_wait32((int *)&worker_ready, 0, -1);
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
  *next_delay_ms = 17;
  if (frame_count++ == 0) {
    oos_platform_subruntime_error_code_t completion_error;
    (void)oos_platform_subruntime_complete(7, &completion_error);
  }
  return true;
}

void exports_oos_platform_lifecycle_shutdown(void) {
  if (worker_started) {
    atomic_store_explicit(&worker_stop, 1, memory_order_release);
    __builtin_wasm_memory_atomic_notify((int *)&worker_stop, 1);
    if (pthread_join(worker_thread, NULL) != 0)
      log_error("subruntime child pthread_join failed");
  }
  free(working_set);
  working_set = NULL;
  worker_started = 0;
  atomic_store_explicit(&worker_ready, 0, memory_order_relaxed);
  atomic_store_explicit(&worker_stop, 0, memory_order_relaxed);
  frame_count = 0;
}
