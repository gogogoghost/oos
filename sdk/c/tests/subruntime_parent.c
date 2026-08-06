#include "app.h"

#include <stdint.h>

static void log_child_error(void) {
  app_string_t message = {0};
  oos_platform_subruntime_last_error(&message);
  if (message.len)
    oos_platform_runtime_log(OOS_PLATFORM_RUNTIME_LOG_LEVEL_ERROR, &message);
  app_string_free(&message);
}

static bool
exercise_expected_failure(const char *name, size_t name_length,
                          uint64_t memory_limit,
                          exports_oos_platform_lifecycle_error_code_t *error) {
  app_string_t module = {(uint8_t *)name, name_length};
  uint32_t handle = 0;
  if (!oos_platform_subruntime_create(&module, 512 * 1024, 2 * 1024 * 1024,
                                      memory_limit, &handle, error)) {
    log_child_error();
    return false;
  }
  if (oos_platform_subruntime_initialize(handle, error)) {
    oos_platform_runtime_log(OOS_PLATFORM_RUNTIME_LOG_LEVEL_ERROR, &module);
    app_string_t unexpected = {(uint8_t *)"expected child failure succeeded",
                               32};
    oos_platform_runtime_log(OOS_PLATFORM_RUNTIME_LOG_LEVEL_ERROR, &unexpected);
    oos_platform_subruntime_destroy(handle, error);
    return false;
  }
  if (!oos_platform_subruntime_destroy(handle, error)) {
    log_child_error();
    return false;
  }
  return true;
}

bool exports_oos_platform_lifecycle_init(
    exports_oos_platform_lifecycle_error_code_t *error) {
  app_string_t module = {(uint8_t *)"memory-child", 12};
  if (!exercise_expected_failure("allocation-failure", 18, 16ULL * 1024 * 1024,
                                 error) ||
      !exercise_expected_failure("trap-child", 10, 64ULL * 1024 * 1024,
                                 error) ||
      !exercise_expected_failure("stack-overflow", 14, 64ULL * 1024 * 1024,
                                 error))
    return false;
  for (unsigned iteration = 0; iteration < 12; ++iteration) {
    uint32_t handle = 0;
    uint32_t delay = 0;
    if (!oos_platform_subruntime_create(&module, 512 * 1024, 2 * 1024 * 1024,
                                        32ULL * 1024 * 1024, &handle, error) ||
        !oos_platform_subruntime_initialize(handle, error) ||
        !oos_platform_subruntime_frame(
            handle, oos_platform_runtime_monotonic_time_us(), &delay, error) ||
        delay != 17 || !oos_platform_subruntime_destroy(handle, error)) {
      log_child_error();
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
