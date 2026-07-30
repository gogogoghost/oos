#include "audio_thread_priority.h"

#include <glib.h>

#include <dirent.h>
#include <sys/resource.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

namespace {

constexpr int kDefaultAudioNice = -10;
constexpr guint kScanIntervalMilliseconds = 250;

std::unordered_set<pid_t> g_configured_threads;

int configured_audio_nice() {
  const char *configured = std::getenv("OOS_WEB_AUDIO_NICE");
  if (!configured || !configured[0])
    return kDefaultAudioNice;

  errno = 0;
  char *end = nullptr;
  const long value = std::strtol(configured, &end, 10);
  if (errno || !end || end[0] || value < -20 || value > 19)
    return kDefaultAudioNice;
  return static_cast<int>(value);
}

bool is_audio_thread(const char *name) {
  return !std::strncmp(name, "webaudioSrcTask", 15) ||
         !std::strncmp(name, "webaudioSrc:src", 15) ||
         !std::strcmp(name, "AudioTrack");
}

bool read_thread_name(pid_t tid, char *name, size_t capacity) {
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
  FILE *file = std::fopen(path, "r");
  if (!file)
    return false;
  const bool read = std::fgets(name, capacity, file) != nullptr;
  std::fclose(file);
  if (!read)
    return false;
  name[std::strcspn(name, "\r\n")] = '\0';
  return true;
}

gboolean prioritize_audio_threads(gpointer) {
  const int target_nice = configured_audio_nice();
  if (target_nice >= 0)
    return G_SOURCE_CONTINUE;

  DIR *tasks = opendir("/proc/self/task");
  if (!tasks)
    return G_SOURCE_CONTINUE;

  while (dirent *entry = readdir(tasks)) {
    char *end = nullptr;
    const long value = std::strtol(entry->d_name, &end, 10);
    if (!end || end == entry->d_name || end[0] || value <= 0)
      continue;
    const auto tid = static_cast<pid_t>(value);
    if (g_configured_threads.count(tid))
      continue;

    char name[32]{};
    if (!read_thread_name(tid, name, sizeof(name)) || !is_audio_thread(name))
      continue;

    errno = 0;
    const int current_nice = getpriority(PRIO_PROCESS, tid);
    if (!errno && current_nice > target_nice &&
        setpriority(PRIO_PROCESS, tid, target_nice) != 0) {
      std::fprintf(stderr,
                   "OOS WebAudio priority failed: tid=%d name=%s nice=%d "
                   "error=%s\n",
                   tid, name, target_nice, std::strerror(errno));
    }
    g_configured_threads.insert(tid);
  }
  closedir(tasks);
  return G_SOURCE_CONTINUE;
}

} // namespace

void install_audio_thread_priority_manager() {
  prioritize_audio_threads(nullptr);
  const guint source = g_timeout_add(kScanIntervalMilliseconds,
                                     prioritize_audio_threads, nullptr);
  g_source_set_name_by_id(source, "OOS WebAudio thread priority");
}
