#include <string.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE // *_np
#endif
#ifdef _3DS
#include <3ds/svc.h>
#include <3ds/os.h>
#include <3ds/services/apt.h>
#include <sys/time.h>
#include "../libpcsxcore/new_dynarec/new_dynarec.h"
static bool is_new_3ds;
#endif
#include "pcsxr-threads.h"

int pcsxr_sthread_core_count;

// pcsxr "extensions"
extern void SysPrintf(const char *fmt, ...);

#if defined(__lv2ppu__)

/* PS3 (PSL1GHT): lv2 PPU threads, mutexes and condition variables. lv2 binds a
 * condition variable to one mutex at creation, so it is created at the first
 * wait, with the mutex used there (the core always pairs each cond with one lock). */
#include <stdlib.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/cond.h>
void sysThreadExit(u64 ret_val);

struct sthread { sys_ppu_thread_t id; void (*fn)(void *); };
struct slock { sys_mutex_t m; };
struct scond { sys_cond_t c; volatile int bound; };

static void lv2_thread_entry(void *arg)
{
	struct sthread *t = arg;
	t->fn(NULL);
	sysThreadExit(0);
}

sthread_t *pcsxr_sthread_create(void (*thread_func)(void *), enum pcsxr_thread_type type)
{
	static char *names[PCSXRT_COUNT] = { "pcsxr-cdrom", "pcsxr-drc", "pcsxr-gpu", "pcsxr-spu" };
	struct sthread *t = calloc(1, sizeof(*t));
	if (!t)
		return NULL;
	t->fn = thread_func;
	/* same priority as the main thread (SYS_PROCESS_PARAM 1001): lv2 runs the
	   two on the PPU's two hardware threads */
	if (sysThreadCreate(&t->id, lv2_thread_entry, t, 1001, 256 * 1024, THREAD_JOINABLE,
	                    names[(unsigned)type < PCSXRT_COUNT ? type : PCSXRT_GPU]) != 0) {
		SysPrintf("sysThreadCreate failed\n");
		free(t);
		return NULL;
	}
	return t;
}

void sthread_join(sthread_t *t)
{
	u64 ret;
	if (!t)
		return;
	sysThreadJoin(t->id, &ret);
	free(t);
}

slock_t *slock_new(void)
{
	struct slock *l = calloc(1, sizeof(*l));
	sys_mutex_attr_t a;
	if (!l)
		return NULL;
	memset(&a, 0, sizeof(a));
	a.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO;
	a.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
	a.attr_pshared = SYS_MUTEX_ATTR_NOT_PSHARED;
	a.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
	if (sysMutexCreate(&l->m, &a) != 0) {
		free(l);
		return NULL;
	}
	return l;
}

void slock_free(slock_t *l)
{
	if (!l)
		return;
	sysMutexDestroy(l->m);
	free(l);
}

void slock_lock(slock_t *l) { sysMutexLock(l->m, 0); }
void slock_unlock(slock_t *l) { sysMutexUnlock(l->m); }

scond_t *scond_new(void) { return calloc(1, sizeof(struct scond)); }

void scond_free(scond_t *c)
{
	if (!c)
		return;
	if (c->bound)
		sysCondDestroy(c->c);
	free(c);
}

void scond_wait(scond_t *c, slock_t *l)
{
	if (!c->bound) {          /* the caller holds l */
		sys_cond_attr_t a;
		memset(&a, 0, sizeof(a));
		a.attr_pshared = SYS_COND_ATTR_PSHARED;
		if (sysCondCreate(&c->c, l->m, &a) != 0) {
			SysPrintf("sysCondCreate failed\n");
			return;
		}
		__asm__ volatile("lwsync" ::: "memory");
		c->bound = 1;
	}
	sysCondWait(c->c, 0);
}

/* a signal before the first wait has no waiter to wake: the waiter checks its
   predicate under the lock before waiting */
void scond_signal(scond_t *c)
{
	if (c->bound)
		sysCondSignal(c->c);
}

#define CORE_COUNT() 2        /* the PPU's two hardware threads */
#define pcsxr_sthread_lib_init()

#elif !defined(USE_C11_THREADS)

#include "../deps/libretro-common/rthreads/rthreads.c"
#include "features/features_cpu.h"

#define CORE_COUNT() cpu_features_get_core_amount()

static void pcsxr_sthread_lib_init(void)
{
#ifdef _3DS
	int64_t version = 0;
	int fpscr = -1;

	APT_CheckNew3DS(&is_new_3ds);
	svcGetSystemInfo(&version, 0x10000, 0);

	APT_SetAppCpuTimeLimit(35);
	u32 percent = -1;
	APT_GetAppCpuTimeLimit(&percent);

	__asm__ volatile("fmrx %0, fpscr" : "=r"(fpscr));
	SysPrintf("%s3ds detected, v%d.%d, AppCpuTimeLimit=%ld fpscr=%08x\n",
		is_new_3ds ? "new" : "old", (int)GET_VERSION_MAJOR(version),
		(int)GET_VERSION_MINOR(version), percent, fpscr);
#endif
}

sthread_t *pcsxr_sthread_create(void (*thread_func)(void *),
	enum pcsxr_thread_type type)
{
	sthread_t *h = NULL;
#ifdef _3DS
	size_t stack_size = 64*1024;
	Thread ctr_thread;
	int core_id = 0;
	s32 prio = 0x30;

	h = calloc(1, sizeof(*h));
	if (!h)
		return NULL;

	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);

	switch (type) {
	case PCSXRT_CDR:
	case PCSXRT_SPU:
		core_id = 1;
		break;
	case PCSXRT_DRC:
		stack_size = new_dynarec_estimate_stack_size();
		// fallthrough
	case PCSXRT_GPU:
		core_id = is_new_3ds ? 2 : 1;
		break;
	case PCSXRT_COUNT:
		break;
	}

	ctr_thread = threadCreate(thread_func, NULL, stack_size, prio, core_id, false);
	if (!ctr_thread) {
		if (core_id == 1) {
			SysPrintf("threadCreate pcsxt %d core %d failed\n",
				type, core_id);
			core_id = is_new_3ds ? 2 : -1;
			ctr_thread = threadCreate(thread_func, NULL, stack_size,
				prio, core_id, false);
		}
	}
	SysPrintf("threadCreate: pcsxt %d core %d stack %zd: %p\n",
		type, core_id, stack_size, ctr_thread);
	if (!ctr_thread) {
		free(h);
		return NULL;
	}
	h->id = ctr_thread;
#else
	h = sthread_create(thread_func, NULL);
 #if defined(__GLIBC__) || \
    (defined(__ANDROID_API__) && __ANDROID_API__ >= 26)
	if (h && (unsigned int)type < (unsigned int)PCSXRT_COUNT)
	{
		const char * const pcsxr_tnames[PCSXRT_COUNT] = {
			"pcsxr-cdrom", "pcsxr-drc", "pcsxr-gpu", "pcsxr-spu"
		};
		pthread_setname_np(h->id, pcsxr_tnames[type]);
	}
 #endif
#endif
	return h;
}

#else // USE_C11_THREADS

#include <unistd.h>
#define CORE_COUNT() sysconf(_SC_NPROCESSORS_ONLN)
#define pcsxr_sthread_lib_init()

#endif

void pcsxr_sthread_init(void)
{
	pcsxr_sthread_core_count = CORE_COUNT();
	if (pcsxr_sthread_core_count < 1)
		pcsxr_sthread_core_count = 1;
	SysPrintf("%d cpu core(s) detected\n", pcsxr_sthread_core_count);

	pcsxr_sthread_lib_init();
}
