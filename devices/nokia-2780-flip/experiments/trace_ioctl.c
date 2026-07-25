#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ARM EABI register layout returned by PTRACE_GETREGS on this 32-bit device. */
struct arm_regs {
  unsigned long uregs[18];
};

enum {
  ARM_R0 = 0,
  ARM_R7 = 7,
  ARM_SYSCALL_IOCTL = 54,
};

static void die(const char *operation) {
  perror(operation);
  exit(1);
}

static bool is_display_fd(int fd) {
  return fd == 26 || fd == 27 || fd == 28 || fd == 82 || fd == 83;
}

static bool first_request(unsigned long request) {
  static unsigned long seen[32];
  for (size_t i = 0; i < 32; ++i) {
    if (seen[i] == request) return false;
    if (seen[i] == 0) {
      seen[i] = request;
      return true;
    }
  }
  return false;
}

static void dump_bytes(pid_t tid, unsigned long address, size_t length) {
  if (length > 64) length = 64;
  fputs("data=", stdout);
  for (size_t offset = 0; offset < length; offset += sizeof(long)) {
    errno = 0;
    long word = ptrace(PTRACE_PEEKDATA, tid, (void *)(address + offset), NULL);
    if (word == -1 && errno != 0) {
      fputs("<unreadable>", stdout);
      break;
    }
    size_t chunk = length - offset < sizeof(word) ? length - offset : sizeof(word);
    for (size_t byte = 0; byte < chunk; ++byte)
      printf("%02x", ((unsigned char *)&word)[byte]);
  }
  putchar('\n');
}

static bool read_u32(pid_t tid, unsigned long address, uint32_t *value) {
  errno = 0;
  long word = ptrace(PTRACE_PEEKDATA, tid, (void *)address, NULL);
  if (word == -1 && errno != 0) return false;
  *value = (uint32_t)word;
  return true;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s PID\n", argv[0]);
    return 2;
  }

  pid_t pid = (pid_t)strtol(argv[1], NULL, 10);
  char task_path[64];
  snprintf(task_path, sizeof(task_path), "/proc/%d/task", pid);
  DIR *tasks = opendir(task_path);
  if (tasks == NULL) die("opendir task");

  pid_t tids[256];
  bool entering[256] = {0};
  unsigned long request[256] = {0};
  size_t count = 0;
  struct dirent *entry;
  while ((entry = readdir(tasks)) != NULL && count < 256) {
    char *end;
    long value = strtol(entry->d_name, &end, 10);
    if (*entry->d_name == '\0' || *end != '\0') continue;
    tids[count++] = (pid_t)value;
  }
  closedir(tasks);

  for (size_t i = 0; i < count; ++i) {
    if (ptrace(PTRACE_ATTACH, tids[i], NULL, NULL) == -1) continue;
    int status;
    if (waitpid(tids[i], &status, __WALL) == -1) continue;
    if (ptrace(PTRACE_SETOPTIONS, tids[i], NULL, PTRACE_O_TRACESYSGOOD) == -1)
      continue;
    entering[i] = true;
    if (ptrace(PTRACE_SYSCALL, tids[i], NULL, NULL) == -1) die("PTRACE_SYSCALL");
  }

  for (;;) {
    int status;
    pid_t tid = waitpid(-1, &status, __WALL);
    if (tid == -1) die("waitpid");
    if (WIFEXITED(status) || WIFSIGNALED(status)) break;
    if (!WIFSTOPPED(status) || (WSTOPSIG(status) & 0x80) == 0) continue;

    size_t i;
    for (i = 0; i < count && tids[i] != tid; ++i) {}
    if (i == count) continue;

    struct arm_regs regs;
    if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) == -1) die("PTRACE_GETREGS");
    if (entering[i] && regs.uregs[ARM_R7] == ARM_SYSCALL_IOCTL &&
        is_display_fd((int)regs.uregs[ARM_R0])) {
      request[i] = regs.uregs[1];
      printf("tid=%d ioctl fd=%d request=0x%08lx arg=0x%08lx ", tid,
             (int)regs.uregs[ARM_R0], request[i],
             regs.uregs[2]);
      fflush(stdout);
      if (first_request(request[i])) {
        size_t size = (request[i] >> 16) & 0x3fff;
        dump_bytes(tid, regs.uregs[2], size);
        if (request[i] == 0xc0084906UL) {
          uint32_t nested;
          if (read_u32(tid, regs.uregs[2] + 4, &nested)) {
            printf("custom_arg=0x%08x ", nested);
            dump_bytes(tid, nested, 32);
          }
        }
      }
    } else if (!entering[i] && request[i] != 0) {
      printf("result=%ld\n", (long)regs.uregs[ARM_R0]);
      fflush(stdout);
      request[i] = 0;
    }
    entering[i] = !entering[i];
    if (ptrace(PTRACE_SYSCALL, tid, NULL, NULL) == -1) die("PTRACE_SYSCALL");
  }
  return 0;
}
