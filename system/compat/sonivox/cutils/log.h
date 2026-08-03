#pragma once

#include <log/log.h>

#ifndef OOS_ANDROID_ERROR_WRITE_LOG_DEFINED
#define OOS_ANDROID_ERROR_WRITE_LOG_DEFINED
static inline int android_errorWriteLog(int tag, const char *message) {
  (void)tag;
  (void)message;
  return 0;
}
#endif
