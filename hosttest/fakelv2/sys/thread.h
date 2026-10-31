#include <stdlib.h>
/* host stand-in for the lv2 calls used by frontend/pcsxr-threads.c (pthreads) */
#pragma once
#include <pthread.h>
#include <stdint.h>
typedef uint64_t u64; typedef int32_t s32; typedef uint32_t u32;
typedef pthread_t sys_ppu_thread_t;
#define THREAD_JOINABLE 1
struct fake_start { void (*e)(void *); void *a; };
static void *fake_tramp(void *p) { struct fake_start s = *(struct fake_start *)p; free(p); s.e(s.a); return 0; }
static inline s32 sysThreadCreate(sys_ppu_thread_t *id, void (*e)(void *), void *a, s32 prio, u64 st, u64 fl, char *name)
{ struct fake_start *s = malloc(sizeof *s); s->e = e; s->a = a; (void)prio; (void)st; (void)fl; (void)name;
  return pthread_create(id, 0, fake_tramp, s); }
static inline s32 sysThreadJoin(sys_ppu_thread_t id, u64 *r) { (void)r; return pthread_join(id, 0); }
void sysThreadExit(u64 r);
static inline void fake_exit_unused(u64 r) { (void)r; pthread_exit(0); }

