#pragma once
#include "thread.h"
typedef pthread_mutex_t *sys_mutex_t;
typedef struct { u32 attr_protocol, attr_recursive, attr_pshared, attr_adaptive; u64 key; s32 flags; u32 _pad; char name[8]; } sys_mutex_attr_t;
#define SYS_MUTEX_PROTOCOL_FIFO 1
#define SYS_MUTEX_ATTR_NOT_RECURSIVE 0x20
#define SYS_MUTEX_ATTR_NOT_PSHARED 0x200
#define SYS_MUTEX_ATTR_NOT_ADAPTIVE 0x2000
static inline s32 sysMutexCreate(sys_mutex_t *m, const sys_mutex_attr_t *a) { (void)a; *m = malloc(sizeof **m); return pthread_mutex_init(*m, 0); }
static inline s32 sysMutexDestroy(sys_mutex_t m) { pthread_mutex_destroy(m); free(m); return 0; }
static inline s32 sysMutexLock(sys_mutex_t m, u64 t) { (void)t; return pthread_mutex_lock(m); }
static inline s32 sysMutexUnlock(sys_mutex_t m) { return pthread_mutex_unlock(m); }
