/* lv2 versions of the POSIX thread calls Lightrec's threaded compiler uses (see
 * pthread.h). Error returns are lv2 codes (non-zero), which is all Lightrec tests. */
#include <stdlib.h>
#include <string.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/cond.h>
#include "pthread.h"

/* the compiler worker: just below the emulator thread (priority 1000), so it uses
   the PPU's second hardware thread without ever pushing the emulation aside */
#define WORKER_PRIORITY 1001
#define WORKER_STACK (512 * 1024)

struct start_box { void *(*start)(void *); void *arg; };

static void trampoline(void *p)
{
	struct start_box b = *(struct start_box *)p;
	free(p);
	b.start(b.arg);
	sysThreadExit(0);
}

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
	struct start_box *b = malloc(sizeof *b);
	sys_ppu_thread_t id;
	s32 r;
	(void)attr;
	if (!b)
		return -1;
	b->start = start; b->arg = arg;
	r = sysThreadCreate(&id, trampoline, b, WORKER_PRIORITY, WORKER_STACK, THREAD_JOINABLE, (char *)"lightrec compiler");
	if (r) {
		free(b);
		return r;
	}
	t->id = id;
	return 0;
}

int pthread_join(pthread_t t, void **ret)
{
	u64 v;
	s32 r = sysThreadJoin(t.id, &v);
	if (ret)
		*ret = NULL;
	return r;
}

static sys_mutex_t new_mutex(void)
{
	sys_mutex_attr_t a;
	sys_mutex_t m = 0;
	memset(&a, 0, sizeof a);
	a.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;
	a.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
	a.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
	a.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
	memcpy(a.name, "lightrc", 8);
	return sysMutexCreate(&m, &a) ? 0 : m;
}

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
	(void)attr;
	m->id = new_mutex();
	return m->id ? 0 : -1;
}
int pthread_mutex_destroy(pthread_mutex_t *m) { return sysMutexDestroy(m->id); }
int pthread_mutex_lock(pthread_mutex_t *m) { return sysMutexLock(m->id, 0); }
int pthread_mutex_unlock(pthread_mutex_t *m) { return sysMutexUnlock(m->id); }

/* guards making condition variables (two threads may wait on a new one at once) */
static sys_mutex_t make_lock;

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr)
{
	(void)attr;
	c->id = c->mutex = 0;
	c->made = 0;
	if (!make_lock)                  /* first use: Lightrec initialises on one thread */
		make_lock = new_mutex();
	return make_lock ? 0 : -1;
}

int pthread_cond_destroy(pthread_cond_t *c)
{
	if (c->made)
		sysCondDestroy(c->id);
	c->made = 0;
	return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
	if (!c->made) {
		sysMutexLock(make_lock, 0);
		if (!c->made) {
			sys_cond_attr_t a;
			memset(&a, 0, sizeof a);
			a.attr_pshared = SYS_COND_ATTR_PSHARED;   /* 0x200, as sysCondAttrInitialize sets it (lv2: not process-shared) */
			memcpy(a.name, "lightrc", 8);
			if (sysCondCreate(&c->id, m->id, &a)) {
				sysMutexUnlock(make_lock);
				return -1;
			}
			c->mutex = m->id;
			__asm__ volatile("lwsync" : : : "memory");
			c->made = 1;
		}
		sysMutexUnlock(make_lock);
	}
	return sysCondWait(c->id, 0);
}

/* nobody can be waiting on one not made yet: a waiter makes it while holding the
   mutex, and Lightrec signals with that mutex held */
int pthread_cond_signal(pthread_cond_t *c) { return c->made ? sysCondSignal(c->id) : 0; }
int pthread_cond_broadcast(pthread_cond_t *c) { return c->made ? sysCondBroadcast(c->id) : 0; }
