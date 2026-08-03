#pragma once

#define ALOGV(...) ((void)0)
#define ALOGD(...) ((void)0)
#define ALOGI(...) ((void)0)
#define ALOGW(...) ((void)0)
#define ALOGE(...) ((void)0)

#ifndef OOS_ANDROID_ERROR_WRITE_LOG_DEFINED
#define OOS_ANDROID_ERROR_WRITE_LOG_DEFINED
static inline int android_errorWriteLog(int tag, const char *message) {
  (void)tag;
  (void)message;
  return 0;
}
#endif
