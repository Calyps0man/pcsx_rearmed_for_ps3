#pragma once
#include "mutex.h"
typedef struct { pthread_cond_t c; pthread_mutex_t *m; } *sys_cond_t;
typedef struct { u32 attr_pshared; s32 flags; u64 key; char name[8]; } sys_cond_attr_t;
#define SYS_COND_ATTR_PSHARED 0x200
static inline s32 sysCondCreate(sys_cond_t *c, sys_mutex_t m, const sys_cond_attr_t *a) { (void)a; *c = malloc(sizeof **c); (*c)->m = m; return pthread_cond_init(&(*c)->c, 0); }
static inline s32 sysCondDestroy(sys_cond_t c) { pthread_cond_destroy(&c->c); free(c); return 0; }
static inline s32 sysCondWait(sys_cond_t c, u64 t) { (void)t; return pthread_cond_wait(&c->c, c->m); }
static inline s32 sysCondSignal(sys_cond_t c) { return pthread_cond_signal(&c->c); }
