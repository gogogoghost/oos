/*
 * WAMR pthread guest ABI.
 * Derived from WAMR's Apache-2.0 WITH LLVM-exception SDK declarations.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OOS_WAMR_IMPORT(name)                                                \
  __attribute__((import_module("env"), import_name(#name)))

typedef unsigned int pthread_t;
typedef unsigned int pthread_mutex_t;
typedef unsigned int pthread_cond_t;
typedef unsigned int pthread_key_t;

OOS_WAMR_IMPORT(pthread_create)
int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
OOS_WAMR_IMPORT(pthread_join)
int pthread_join(pthread_t, void **);
OOS_WAMR_IMPORT(pthread_detach)
int pthread_detach(pthread_t);
OOS_WAMR_IMPORT(pthread_cancel)
int pthread_cancel(pthread_t);
OOS_WAMR_IMPORT(pthread_self)
pthread_t pthread_self(void);
OOS_WAMR_IMPORT(pthread_exit)
void pthread_exit(void *);
OOS_WAMR_IMPORT(pthread_mutex_init)
int pthread_mutex_init(pthread_mutex_t *, const void *);
OOS_WAMR_IMPORT(pthread_mutex_lock)
int pthread_mutex_lock(pthread_mutex_t *);
OOS_WAMR_IMPORT(pthread_mutex_unlock)
int pthread_mutex_unlock(pthread_mutex_t *);
OOS_WAMR_IMPORT(pthread_mutex_destroy)
int pthread_mutex_destroy(pthread_mutex_t *);
OOS_WAMR_IMPORT(pthread_cond_init)
int pthread_cond_init(pthread_cond_t *, const void *);
OOS_WAMR_IMPORT(pthread_cond_wait)
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
OOS_WAMR_IMPORT(pthread_cond_timedwait)
int pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, uint64_t);
OOS_WAMR_IMPORT(pthread_cond_signal)
int pthread_cond_signal(pthread_cond_t *);
OOS_WAMR_IMPORT(pthread_cond_broadcast)
int pthread_cond_broadcast(pthread_cond_t *);
OOS_WAMR_IMPORT(pthread_cond_destroy)
int pthread_cond_destroy(pthread_cond_t *);
OOS_WAMR_IMPORT(pthread_key_create)
int pthread_key_create(pthread_key_t *, void (*)(void *));
OOS_WAMR_IMPORT(pthread_setspecific)
int pthread_setspecific(pthread_key_t, const void *);
OOS_WAMR_IMPORT(pthread_getspecific)
void *pthread_getspecific(pthread_key_t);
OOS_WAMR_IMPORT(pthread_key_delete)
int pthread_key_delete(pthread_key_t);

#undef OOS_WAMR_IMPORT

#ifdef __cplusplus
}
#endif
