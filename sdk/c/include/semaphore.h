/* WAMR semaphore guest ABI. Apache-2.0 WITH LLVM-exception. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int sem_t;

#define OOS_WAMR_IMPORT(name)                                                  \
  __attribute__((import_module("env"), import_name(#name)))

OOS_WAMR_IMPORT(sem_open)
sem_t *sem_open(const char *, int, int, int);
OOS_WAMR_IMPORT(sem_wait)
int sem_wait(sem_t *);
OOS_WAMR_IMPORT(sem_trywait)
int sem_trywait(sem_t *);
OOS_WAMR_IMPORT(sem_post)
int sem_post(sem_t *);
OOS_WAMR_IMPORT(sem_getvalue)
int sem_getvalue(sem_t *, int *);
OOS_WAMR_IMPORT(sem_unlink)
int sem_unlink(const char *);
OOS_WAMR_IMPORT(sem_close)
int sem_close(sem_t *);

#undef OOS_WAMR_IMPORT

#ifdef __cplusplus
}
#endif
