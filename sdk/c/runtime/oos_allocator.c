#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <tlsf.h>

extern unsigned char __heap_base;

typedef struct {
  size_t current_bytes;
  size_t peak_bytes;
  size_t allocation_count;
  size_t free_count;
  size_t linear_memory_bytes;
} oos_allocator_stats_t;

static tlsf_t allocator;
static _Atomic unsigned allocator_lock;
static oos_allocator_stats_t statistics;

// TLSF diagnostics describe invalid host configuration. OOS validates the
// pool before calling TLSF, so production guests keep this path allocation and
// I/O free instead of importing a WASI console.
int oos_tlsf_diagnostic(const char *format, ...) {
  (void)format;
  return 0;
}

static void lock(void) {
  while (__c11_atomic_exchange(&allocator_lock, 1, __ATOMIC_ACQUIRE))
    __builtin_wasm_memory_atomic_wait32((int *)&allocator_lock, 1, -1);
}

static void unlock(void) {
  __c11_atomic_store(&allocator_lock, 0, __ATOMIC_RELEASE);
  __builtin_wasm_memory_atomic_notify((int *)&allocator_lock, 1);
}

static int initialize(void) {
  if (allocator)
    return 1;
  const uintptr_t alignment = tlsf_align_size();
  const uintptr_t begin =
      ((uintptr_t)&__heap_base + alignment - 1) & ~(alignment - 1);
  const uintptr_t end = __builtin_wasm_memory_size(0) * 65536u;
  if (end <= begin || end - begin < tlsf_size() + tlsf_pool_overhead())
    return 0;
  allocator = tlsf_create_with_pool((void *)begin, end - begin);
  statistics.linear_memory_bytes = end;
  return allocator != NULL;
}

static int grow(size_t requested) {
  const size_t overhead = tlsf_pool_overhead() + tlsf_alloc_overhead();
  if (requested > SIZE_MAX - overhead)
    return 0;
  const size_t required = requested + overhead;
  if (required > SIZE_MAX - 65535u)
    return 0;
  const size_t pages =
      required > 16 * 65536u ? (required + 65535u) / 65536u : 16;
  const size_t previous = __builtin_wasm_memory_grow(0, pages);
  if (previous == (size_t)-1)
    return 0;
  void *pool = (void *)(previous * 65536u);
  if (!tlsf_add_pool(allocator, pool, pages * 65536u))
    return 0;
  statistics.linear_memory_bytes = (previous + pages) * 65536u;
  return 1;
}

void *malloc(size_t size) {
  if (size == 0)
    size = 1;
  lock();
  if (!initialize()) {
    unlock();
    return NULL;
  }
  void *result = tlsf_malloc(allocator, size);
  while (!result && grow(size))
    result = tlsf_malloc(allocator, size);
  if (result) {
    statistics.current_bytes += tlsf_block_size(result);
    if (statistics.current_bytes > statistics.peak_bytes)
      statistics.peak_bytes = statistics.current_bytes;
    ++statistics.allocation_count;
  }
  unlock();
  return result;
}

void free(void *pointer) {
  if (!pointer)
    return;
  lock();
  statistics.current_bytes -= tlsf_block_size(pointer);
  ++statistics.free_count;
  tlsf_free(allocator, pointer);
  unlock();
}

void *calloc(size_t count, size_t size) {
  if (count && size > SIZE_MAX / count)
    return NULL;
  const size_t total = count * size;
  void *result = malloc(total);
  if (result)
    memset(result, 0, total);
  return result;
}

void *realloc(void *pointer, size_t size) {
  if (!pointer)
    return malloc(size);
  if (!size) {
    free(pointer);
    return NULL;
  }
  lock();
  const size_t previous_size = tlsf_block_size(pointer);
  void *result = tlsf_realloc(allocator, pointer, size);
  while (!result && grow(size))
    result = tlsf_realloc(allocator, pointer, size);
  if (result) {
    const size_t current_size = tlsf_block_size(result);
    statistics.current_bytes =
        statistics.current_bytes - previous_size + current_size;
    if (statistics.current_bytes > statistics.peak_bytes)
      statistics.peak_bytes = statistics.current_bytes;
  }
  unlock();
  return result;
}

void oos_allocator_get_stats(oos_allocator_stats_t *result) {
  if (!result)
    return;
  lock();
  *result = statistics;
  unlock();
}

_Noreturn void abort(void) { __builtin_trap(); }
