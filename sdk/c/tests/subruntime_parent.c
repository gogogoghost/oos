#include "app.h"

#include <stdint.h>

static uint32_t child_handle;
static unsigned phase;
static unsigned iteration;
static unsigned failure_index;
static const char *failure_modules[] = {"allocation-failure", "trap-child",
                                        "stack-overflow"};

static void log_child_error(void) {
  app_string_t message = {0};
  oos_platform_subruntime_last_error(&message);
  if (message.len)
    oos_platform_runtime_log(OOS_PLATFORM_RUNTIME_LOG_LEVEL_ERROR, &message);
  app_string_free(&message);
}

static bool create_child(const char *name,
                         exports_oos_platform_lifecycle_error_code_t *error) {
  app_string_t module;
  app_string_set(&module, name);
  return oos_platform_subruntime_create(&module, 512 * 1024, &child_handle,
                                        error) &&
         oos_platform_subruntime_initialize(child_handle, error);
}

bool exports_oos_platform_lifecycle_init(
    exports_oos_platform_lifecycle_error_code_t *error) {
  if (!create_child("memory-child", error)) {
    log_child_error();
    return false;
  }
  phase = 0;
  iteration = 0;
  failure_index = 0;
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
  oos_platform_subruntime_child_status_t status = {0};
  if (!oos_platform_subruntime_status(child_handle, &status, error)) {
    log_child_error();
    return false;
  }
  if (iteration >= 12) {
    if (status.state != OOS_PLATFORM_SUBRUNTIME_CHILD_STATE_FAILED) {
      *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
      return false;
    }
    if (!oos_platform_subruntime_destroy(child_handle, error))
      return false;
    if (++failure_index <
        sizeof(failure_modules) / sizeof(failure_modules[0])) {
      if (!create_child(failure_modules[failure_index], error))
        return false;
      *next_delay_ms = 0;
      return true;
    }
    phase = 1;
    *next_delay_ms = 1000;
    return true;
  }
  if (phase == 0) {
    if (status.state != OOS_PLATFORM_SUBRUNTIME_CHILD_STATE_COMPLETED ||
        status.result_code != 7) {
      app_string_t message;
      app_string_set(&message, "subruntime child completion state is invalid");
      oos_platform_runtime_log(OOS_PLATFORM_RUNTIME_LOG_LEVEL_ERROR, &message);
      *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
      return false;
    }
    const uint32_t stale_handle = child_handle;
    if (!oos_platform_subruntime_destroy(child_handle, error))
      return false;
    if (oos_platform_subruntime_status(stale_handle, &status, error)) {
      app_string_t message;
      app_string_set(&message, "stale child handle remained valid");
      oos_platform_runtime_log(OOS_PLATFORM_RUNTIME_LOG_LEVEL_ERROR, &message);
      *error = OOS_PLATFORM_TYPES_ERROR_CODE_FAILED;
      return false;
    }
    if (++iteration < 12) {
      if (!create_child("memory-child", error)) {
        log_child_error();
        return false;
      }
      *next_delay_ms = 0;
      return true;
    }
    if (!create_child(failure_modules[0], error))
      return false;
    *next_delay_ms = 0;
    return true;
  }
  *next_delay_ms = 1000;
  return true;
}

void exports_oos_platform_lifecycle_shutdown(void) {
  if (phase == 0) {
    exports_oos_platform_lifecycle_error_code_t error;
    oos_platform_subruntime_destroy(child_handle, &error);
  }
}
