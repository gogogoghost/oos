#include <stddef.h>
#include <stdint.h>

typedef struct block {
  size_t size;
  struct block *next;
  int free;
  uint32_t reserved;
} block_t;

_Static_assert(sizeof(block_t) % 8 == 0,
               "allocator headers must preserve 8-byte alignment");

extern unsigned char __heap_base;
static block_t *blocks;
static _Atomic unsigned allocator_lock;

static void lock(void) {
  while (__c11_atomic_exchange(&allocator_lock, 1, __ATOMIC_ACQUIRE))
    __builtin_wasm_memory_atomic_wait32((int *)&allocator_lock, 1, -1);
}

static void unlock(void) {
  __c11_atomic_store(&allocator_lock, 0, __ATOMIC_RELEASE);
  __builtin_wasm_memory_atomic_notify((int *)&allocator_lock, 1);
}

static size_t align8(size_t size) { return (size + 7) & ~(size_t)7; }

static int grow_to(uintptr_t end) {
  const size_t page = 65536;
  const size_t current = __builtin_wasm_memory_size(0) * page;
  if (end <= current)
    return 1;
  const size_t pages = (end - current + page - 1) / page;
  return __builtin_wasm_memory_grow(0, pages) != (size_t)-1;
}

static block_t *last_block(void) {
  block_t *block = blocks;
  while (block && block->next)
    block = block->next;
  return block;
}

void *malloc(size_t requested) {
  if (!requested)
    requested = 1;
  const size_t size = align8(requested);
  if (size < requested)
    return NULL;
  lock();
  for (block_t *block = blocks; block; block = block->next) {
    if (!block->free || block->size < size)
      continue;
    if (block->size >= size + sizeof(block_t) + 8) {
      block_t *tail = (block_t *)((unsigned char *)(block + 1) + size);
      *tail =
          (block_t){block->size - size - sizeof(block_t), block->next, 1, 0};
      block->next = tail;
      block->size = size;
    }
    block->free = 0;
    unlock();
    return block + 1;
  }
  block_t *last = last_block();
  uintptr_t address = last ? (uintptr_t)(last + 1) + last->size
                           : align8((uintptr_t)&__heap_base);
  if (!grow_to(address + sizeof(block_t) + size)) {
    unlock();
    return NULL;
  }
  block_t *block = (block_t *)address;
  *block = (block_t){size, NULL, 0, 0};
  if (last)
    last->next = block;
  else
    blocks = block;
  unlock();
  return block + 1;
}

void free(void *pointer) {
  if (!pointer)
    return;
  lock();
  block_t *block = (block_t *)pointer - 1;
  block->free = 1;
  for (block_t *current = blocks; current && current->next;) {
    if (current->free && current->next->free &&
        (unsigned char *)(current + 1) + current->size ==
            (unsigned char *)current->next) {
      current->size += sizeof(block_t) + current->next->size;
      current->next = current->next->next;
    } else {
      current = current->next;
    }
  }
  unlock();
}

void *calloc(size_t count, size_t size) {
  if (count && size > SIZE_MAX / count)
    return NULL;
  const size_t total = count * size;
  unsigned char *result = malloc(total);
  if (result)
    for (size_t index = 0; index < total; ++index)
      result[index] = 0;
  return result;
}

void *realloc(void *pointer, size_t size) {
  if (!pointer)
    return malloc(size);
  if (!size) {
    free(pointer);
    return NULL;
  }
  block_t *block = (block_t *)pointer - 1;
  if (block->size >= size)
    return pointer;
  void *replacement = malloc(size);
  if (!replacement)
    return NULL;
  for (size_t index = 0; index < block->size; ++index)
    ((unsigned char *)replacement)[index] = ((unsigned char *)pointer)[index];
  free(pointer);
  return replacement;
}

_Noreturn void abort(void) { __builtin_trap(); }
