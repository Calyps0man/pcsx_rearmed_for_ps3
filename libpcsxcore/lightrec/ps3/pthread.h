/* The POSIX thread calls Lightrec's threaded compiler uses (recompiler.c, reaper.c),
 * for the PS3: PSL1GHT's C library declares no threads. Found before the system's
 * pthread.h only by Lightrec's files (Makefile.libretro, psl1ght). Built on lv2's
 * PPU threads, mutexes and condition variables (pthread.c). */
#ifndef PS3_LIGHTREC_PTHREAD_H
#define PS3_LIGHTREC_PTHREAD_H
#include <stdint.h>

typedef struct { uint64_t id; } pthread_t;            /* lv2 PPU thread id */
typedef struct { uint32_t id; } pthread_mutex_t;      /* lv2 mutex */
/* lv2 ties a condition variable to its mutex when it is created: made at the
   first wait, with the mutex that wait is given */
typedef struct { uint32_t id, mutex; volatile int made; } pthread_cond_t;
typedef int pthread_attr_t, pthread_mutexattr_t, pthread_condattr_t;

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*start)(void *), void *arg);
int pthread_join(pthread_t t, void **ret);
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
#endif
