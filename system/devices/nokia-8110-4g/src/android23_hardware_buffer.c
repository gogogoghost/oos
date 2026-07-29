#include <EGL/egl.h>
#include <android/hardware_buffer.h>
#include <cutils/native_handle.h>
#include <hardware/gralloc.h>
#include <hardware/hardware.h>
#include <linux/ashmem.h>
#include <system/window.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

// The unified NDK header keeps the types visible at API 23 but hides API 26
// function declarations. This library intentionally backports those symbols.
void AHardwareBuffer_acquire(AHardwareBuffer *buffer);
void AHardwareBuffer_release(AHardwareBuffer *buffer);

// WPE 2.52 uses the API 26 NDK shared-memory surface. Android 6 provides the
// same kernel facility through /dev/ashmem, so expose the missing ABI symbols
// without requiring a newer platform libc.
int ASharedMemory_create(const char *name, size_t size) {
  if (size == 0) {
    errno = EINVAL;
    return -1;
  }
  int fd = open("/dev/ashmem", O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return -1;
  if ((name && name[0] && ioctl(fd, ASHMEM_SET_NAME, name) != 0) ||
      ioctl(fd, ASHMEM_SET_SIZE, size) != 0) {
    const int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
  }
  return fd;
}

int ASharedMemory_setProt(int fd, int prot) {
  return ioctl(fd, ASHMEM_SET_PROT_MASK, (unsigned long)prot);
}

int32_t ANativeWindow_getFormat(ANativeWindow *window) {
  int format = 0;
  if (!window || !window->query)
    return -EINVAL;
  const int result = window->query(window, NATIVE_WINDOW_FORMAT, &format);
  return result < 0 ? result : format;
}

enum {
  OOS_BUFFER_WIRE_MAGIC = 0x4f4f5342,
  OOS_BUFFER_WIRE_VERSION = 1,
  OOS_BUFFER_MAX_FDS = 16,
  OOS_BUFFER_MAX_INTS = 128,
};

typedef struct {
  uint32_t magic;
  uint32_t version;
  AHardwareBuffer_Desc desc;
  int32_t num_fds;
  int32_t num_ints;
  int32_t handle_ints[OOS_BUFFER_MAX_INTS];
} OosBufferWire;

struct AHardwareBuffer {
  struct ANativeWindowBuffer native_buffer;
  atomic_int references;
  AHardwareBuffer_Desc description;
  const gralloc_module_t *gralloc;
  alloc_device_t *allocator;
  native_handle_t *received_handle;
  int owns_allocation;
};

static pthread_once_t g_gralloc_once = PTHREAD_ONCE_INIT;
static const gralloc_module_t *g_gralloc;
static alloc_device_t *g_allocator;
static int g_gralloc_error;

static void oos_open_gralloc(void) {
  const hw_module_t *module = NULL;
  g_gralloc_error = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module);
  if (g_gralloc_error || !module)
    return;
  g_gralloc = (const gralloc_module_t *)module;
  g_gralloc_error = gralloc_open(module, &g_allocator);
}

static int oos_require_gralloc(void) {
  pthread_once(&g_gralloc_once, oos_open_gralloc);
  return g_gralloc_error ? g_gralloc_error
                         : (!g_gralloc || !g_allocator ? -ENODEV : 0);
}

static void oos_native_inc_ref(android_native_base_t *base) {
  AHardwareBuffer_acquire((AHardwareBuffer *)base);
}

static void oos_native_dec_ref(android_native_base_t *base) {
  AHardwareBuffer_release((AHardwareBuffer *)base);
}

static int oos_gralloc_usage(uint64_t usage) {
  int result = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE |
               GRALLOC_USAGE_HW_COMPOSER;
  if (usage & AHARDWAREBUFFER_USAGE_CPU_READ_MASK)
    result |= GRALLOC_USAGE_SW_READ_OFTEN;
  if (usage & AHARDWAREBUFFER_USAGE_CPU_WRITE_MASK)
    result |= GRALLOC_USAGE_SW_WRITE_OFTEN;
  return result;
}

static void oos_initialize_native_buffer(AHardwareBuffer *buffer,
                                         buffer_handle_t handle) {
  memset(&buffer->native_buffer, 0, sizeof(buffer->native_buffer));
  buffer->native_buffer.common.magic = ANDROID_NATIVE_BUFFER_MAGIC;
  buffer->native_buffer.common.version = sizeof(struct ANativeWindowBuffer);
  buffer->native_buffer.common.incRef = oos_native_inc_ref;
  buffer->native_buffer.common.decRef = oos_native_dec_ref;
  buffer->native_buffer.width = (int)buffer->description.width;
  buffer->native_buffer.height = (int)buffer->description.height;
  buffer->native_buffer.stride = (int)buffer->description.stride;
  buffer->native_buffer.format = (int)buffer->description.format;
  buffer->native_buffer.usage = oos_gralloc_usage(buffer->description.usage);
  buffer->native_buffer.handle = handle;
}

int AHardwareBuffer_allocate(const AHardwareBuffer_Desc *description,
                             AHardwareBuffer **out_buffer) {
  if (!description || !out_buffer || !description->width ||
      !description->height || description->layers != 1)
    return -EINVAL;
  int result = oos_require_gralloc();
  if (result)
    return result;

  AHardwareBuffer *buffer = calloc(1, sizeof(*buffer));
  if (!buffer)
    return -ENOMEM;
  buffer->description = *description;
  buffer->description.rfu0 = 0;
  buffer->description.rfu1 = 0;
  buffer_handle_t handle = NULL;
  int stride = 0;
  result = g_allocator->alloc(
      g_allocator, (int)description->width, (int)description->height,
      (int)description->format, oos_gralloc_usage(description->usage), &handle,
      &stride);
  if (result || !handle) {
    free(buffer);
    return result ? result : -ENOMEM;
  }
  buffer->description.stride = (uint32_t)stride;
  buffer->gralloc = g_gralloc;
  buffer->allocator = g_allocator;
  buffer->owns_allocation = 1;
  atomic_init(&buffer->references, 1);
  oos_initialize_native_buffer(buffer, handle);
  *out_buffer = buffer;
  return 0;
}

void AHardwareBuffer_acquire(AHardwareBuffer *buffer) {
  if (buffer)
    atomic_fetch_add_explicit(&buffer->references, 1, memory_order_relaxed);
}

void AHardwareBuffer_release(AHardwareBuffer *buffer) {
  if (!buffer || atomic_fetch_sub_explicit(&buffer->references, 1,
                                           memory_order_acq_rel) != 1)
    return;
  if (buffer->owns_allocation && buffer->allocator)
    buffer->allocator->free(buffer->allocator, buffer->native_buffer.handle);
  else if (buffer->received_handle) {
    if (buffer->gralloc && buffer->gralloc->unregisterBuffer)
      buffer->gralloc->unregisterBuffer(buffer->gralloc,
                                        buffer->native_buffer.handle);
    native_handle_close(buffer->received_handle);
    native_handle_delete(buffer->received_handle);
  }
  free(buffer);
}

void AHardwareBuffer_describe(const AHardwareBuffer *buffer,
                              AHardwareBuffer_Desc *out_description) {
  if (buffer && out_description)
    *out_description = buffer->description;
}

int AHardwareBuffer_isSupported(const AHardwareBuffer_Desc *description) {
  if (!description || !description->width || !description->height ||
      description->layers != 1)
    return 0;
  switch (description->format) {
  case AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM:
  case AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM:
  case AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM:
  case AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM:
    return oos_require_gralloc() == 0;
  default:
    return 0;
  }
}

int AHardwareBuffer_lock(AHardwareBuffer *buffer, uint64_t usage, int32_t fence,
                         const ARect *rect, void **out_virtual_address) {
  if (!buffer || !out_virtual_address || !buffer->gralloc)
    return -EINVAL;
  if (fence >= 0) {
    struct pollfd poll_fence = {.fd = fence, .events = POLLIN};
    const int wait_result = poll(&poll_fence, 1, 3000);
    close(fence);
    if (wait_result <= 0)
      return wait_result == 0 ? -ETIMEDOUT : -errno;
  }
  const int left = rect ? rect->left : 0;
  const int top = rect ? rect->top : 0;
  const int width =
      rect ? rect->right - rect->left : (int)buffer->description.width;
  const int height =
      rect ? rect->bottom - rect->top : (int)buffer->description.height;
  return buffer->gralloc->lock(buffer->gralloc, buffer->native_buffer.handle,
                               oos_gralloc_usage(usage), left, top, width,
                               height, out_virtual_address);
}

int AHardwareBuffer_unlock(AHardwareBuffer *buffer, int32_t *fence) {
  if (!buffer || !buffer->gralloc)
    return -EINVAL;
  if (fence)
    *fence = -1;
  return buffer->gralloc->unlock(buffer->gralloc, buffer->native_buffer.handle);
}

EGLClientBuffer eglGetNativeClientBufferANDROID(const AHardwareBuffer *buffer) {
  return buffer ? (EGLClientBuffer)&buffer->native_buffer : NULL;
}

EGLClientBuffer
oos_eglGetNativeClientBufferANDROID(const AHardwareBuffer *buffer) {
  return eglGetNativeClientBufferANDROID(buffer);
}

int AHardwareBuffer_sendHandleToUnixSocket(const AHardwareBuffer *buffer,
                                           int socket_fd) {
  if (!buffer || socket_fd < 0 || !buffer->native_buffer.handle)
    return -EINVAL;
  const native_handle_t *handle = buffer->native_buffer.handle;
  if (handle->numFds < 0 || handle->numFds > OOS_BUFFER_MAX_FDS ||
      handle->numInts < 0 || handle->numInts > OOS_BUFFER_MAX_INTS)
    return -E2BIG;

  OosBufferWire wire;
  memset(&wire, 0, sizeof(wire));
  wire.magic = OOS_BUFFER_WIRE_MAGIC;
  wire.version = OOS_BUFFER_WIRE_VERSION;
  wire.desc = buffer->description;
  wire.num_fds = handle->numFds;
  wire.num_ints = handle->numInts;
  memcpy(wire.handle_ints, handle->data + handle->numFds,
         (size_t)handle->numInts * sizeof(int));

  struct iovec iov = {.iov_base = &wire, .iov_len = sizeof(wire)};
  char control[CMSG_SPACE(sizeof(int) * OOS_BUFFER_MAX_FDS)];
  memset(control, 0, sizeof(control));
  struct msghdr message;
  memset(&message, 0, sizeof(message));
  message.msg_iov = &iov;
  message.msg_iovlen = 1;
  if (handle->numFds) {
    message.msg_control = control;
    message.msg_controllen = CMSG_SPACE(sizeof(int) * handle->numFds);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * handle->numFds);
    memcpy(CMSG_DATA(cmsg), handle->data, (size_t)handle->numFds * sizeof(int));
  }
  if (sendmsg(socket_fd, &message, MSG_NOSIGNAL) < 0)
    return -errno;
  return 0;
}

int AHardwareBuffer_recvHandleFromUnixSocket(int socket_fd,
                                             AHardwareBuffer **out_buffer) {
  if (socket_fd < 0 || !out_buffer)
    return -EINVAL;
  OosBufferWire wire;
  memset(&wire, 0, sizeof(wire));
  struct iovec iov = {.iov_base = &wire, .iov_len = sizeof(wire)};
  char control[CMSG_SPACE(sizeof(int) * OOS_BUFFER_MAX_FDS)];
  memset(control, 0, sizeof(control));
  struct msghdr message;
  memset(&message, 0, sizeof(message));
  message.msg_iov = &iov;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  if (recvmsg(socket_fd, &message, MSG_DONTWAIT) < 0)
    return -errno;
  if (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC))
    return -EMSGSIZE;
  if (wire.magic != OOS_BUFFER_WIRE_MAGIC ||
      wire.version != OOS_BUFFER_WIRE_VERSION || wire.num_fds < 0 ||
      wire.num_fds > OOS_BUFFER_MAX_FDS || wire.num_ints < 0 ||
      wire.num_ints > OOS_BUFFER_MAX_INTS)
    return -EPROTO;

  int received_fds[OOS_BUFFER_MAX_FDS];
  int received_count = 0;
  for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message); cmsg;
       cmsg = CMSG_NXTHDR(&message, cmsg)) {
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
      continue;
    received_count = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
    if (received_count > OOS_BUFFER_MAX_FDS)
      received_count = OOS_BUFFER_MAX_FDS;
    memcpy(received_fds, CMSG_DATA(cmsg), (size_t)received_count * sizeof(int));
    break;
  }
  if (received_count != wire.num_fds) {
    for (int i = 0; i < received_count; ++i)
      close(received_fds[i]);
    return -EPROTO;
  }

  int result = oos_require_gralloc();
  int registered = 0;
  native_handle_t *handle = NULL;
  AHardwareBuffer *buffer = NULL;
  if (!result)
    handle = native_handle_create(wire.num_fds, wire.num_ints);
  if (!result && !handle)
    result = -ENOMEM;
  if (!result) {
    memcpy(handle->data, received_fds, (size_t)wire.num_fds * sizeof(int));
    memcpy(handle->data + wire.num_fds, wire.handle_ints,
           (size_t)wire.num_ints * sizeof(int));
    result = g_gralloc->registerBuffer(g_gralloc, handle);
    registered = result == 0;
  }
  if (!result)
    buffer = calloc(1, sizeof(*buffer));
  if (!result && !buffer)
    result = -ENOMEM;
  if (result) {
    if (handle) {
      if (registered && g_gralloc && g_gralloc->unregisterBuffer)
        g_gralloc->unregisterBuffer(g_gralloc, handle);
      native_handle_close(handle);
      native_handle_delete(handle);
    } else {
      for (int i = 0; i < received_count; ++i)
        close(received_fds[i]);
    }
    return result;
  }
  buffer->description = wire.desc;
  buffer->gralloc = g_gralloc;
  buffer->allocator = g_allocator;
  buffer->received_handle = handle;
  atomic_init(&buffer->references, 1);
  oos_initialize_native_buffer(buffer, handle);
  *out_buffer = buffer;
  return 0;
}
