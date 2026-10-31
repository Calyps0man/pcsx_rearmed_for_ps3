/*
 * Copyright (C) 2013-2023  Free Software Foundation, Inc.
 *
 * This file is part of GNU lightning.
 *
 * GNU lightning is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * GNU lightning is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * Authors:
 *	Paulo Cesar Pereira de Andrade
 */

#include <lightning.h>
#include <stdint.h>
#include <lightning/jit_private.h>

/*
 * Prototypes
 */
static void *jit_default_alloc_func(size_t);
#if defined(JIT_MEM_CACHE)
static void jit_zero(void *ptr, size_t size);
#else
#  define jit_zero(ptr, size)	memset(ptr, 0, size)
#endif
static void *jit_default_realloc_func(void*, size_t);
static void jit_default_free_func(void *);

/*
 * Initialization
 */
static jit_alloc_func_ptr jit_alloc_ptr = jit_default_alloc_func;
static jit_realloc_func_ptr jit_realloc_ptr = jit_default_realloc_func;
static jit_free_func_ptr jit_free_ptr = jit_default_free_func;

/*
 * Implementation
 */
jit_pointer_t
jit_memcpy(jit_pointer_t dst, const void * src, jit_word_t size)
{
    if (size)
	return (memcpy(dst, src, size));
    return (dst);
}

jit_pointer_t
jit_memmove(jit_pointer_t dst, const void *src , jit_word_t size)
{
    if (size)
	return (memmove(dst, src, size));
    return (dst);
}

void
jit_set_memory_functions(jit_alloc_func_ptr alloc_ptr,
			 jit_realloc_func_ptr realloc_ptr,
			 jit_free_func_ptr free_ptr)
{
    if (alloc_ptr == NULL)
	alloc_ptr = jit_default_alloc_func;
    if (realloc_ptr == NULL)
	realloc_ptr = jit_default_realloc_func;
    if (free_ptr == NULL)
	free_ptr = jit_default_free_func;
    jit_alloc_ptr = alloc_ptr;
    jit_realloc_ptr = realloc_ptr;
    jit_free_ptr = free_ptr;
}

void
jit_get_memory_functions(jit_alloc_func_ptr *alloc_ptr,
			 jit_realloc_func_ptr *realloc_ptr,
			 jit_free_func_ptr *free_ptr)
{
    *alloc_ptr = jit_alloc_ptr;
    *realloc_ptr = jit_realloc_ptr;
    *free_ptr = jit_free_ptr;
}

/* time spent here (PowerPC timebase ticks), calls and bytes cleared, for the
 * front end's slow-frame log (all threads together) */
unsigned long long jit_mem_ticks, jit_mem_bytes;
unsigned int jit_mem_calls;
#if defined(__powerpc__) || defined(__ppc__) || defined(__PPC__)
#  define JIT_MEM_T0()	unsigned long long _t0 = __builtin_ppc_get_timebase()
#  define JIT_MEM_T1()	__atomic_fetch_add(&jit_mem_ticks, __builtin_ppc_get_timebase() - _t0, __ATOMIC_RELAXED)
#else
#  define JIT_MEM_T0()	do { } while (0)
#  define JIT_MEM_T1()	do { } while (0)
#endif

void
jit_alloc(jit_pointer_t *ptr, jit_word_t size)
{
    JIT_MEM_T0();
    *ptr = (*jit_alloc_ptr)(size);
    jit_zero(*ptr, size);
    JIT_MEM_T1();
    __atomic_fetch_add(&jit_mem_calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&jit_mem_bytes, size, __ATOMIC_RELAXED);
}

void
jit_realloc(jit_pointer_t *ptr, jit_word_t old_size, jit_word_t new_size)
{
    JIT_MEM_T0();
    *ptr = (*jit_realloc_ptr)(*ptr, new_size);
    if (old_size < new_size)
	jit_zero((jit_int8_t*)*ptr + old_size, new_size - old_size);
    JIT_MEM_T1();
    __atomic_fetch_add(&jit_mem_calls, 1, __ATOMIC_RELAXED);
}

void
jit_free(jit_pointer_t *ptr)
{
    if (*ptr) {
	JIT_MEM_T0();
	(*jit_free_ptr)(*ptr);
	*ptr = NULL;
	JIT_MEM_T1();
	__atomic_fetch_add(&jit_mem_calls, 1, __ATOMIC_RELAXED);
    }
}

#if defined(JIT_MEM_CACHE)
/*
 * Each compile makes a jit state and throws it away, allocating and clearing
 * the same buffers every time (about 75 KB, 30 calls per block on the PS3).
 * Freed buffers of 1 KB and more are kept by size and handed out again (a
 * small header holds the size), and buffers are cleared a cache line at a
 * time with dcbz on PowerPC: the C library's memset reads every line from
 * memory before overwriting it. All of Lightning's memory goes through here.
 */
#  define JMC_HDR	16
#  define JMC_MIN	1024
#  define JMC_SIZES	32
#  define JMC_PER	8
static struct { size_t size; unsigned int n; void *p[JMC_PER]; } jmc[JMC_SIZES];
static int jmc_lock;

static inline void jmc_acquire(void)
{
    while (__atomic_test_and_set(&jmc_lock, __ATOMIC_ACQUIRE))
	;
}

static inline void jmc_release(void)
{
    __atomic_clear(&jmc_lock, __ATOMIC_RELEASE);
}

static void *
jit_default_alloc_func(size_t size)
{
    char *p = NULL;
    unsigned int i;

    if (size >= JMC_MIN) {
	jmc_acquire();
	for (i = 0; i < JMC_SIZES; i++) {
	    if (jmc[i].size == size && jmc[i].n) {
		p = jmc[i].p[--jmc[i].n];
		break;
	    }
	}
	jmc_release();
	if (p)
	    return p + JMC_HDR;
    }

    p = malloc(size + JMC_HDR);
    if (!p)
	return NULL;
    *(size_t *)p = size;
    return p + JMC_HDR;
}

static void *
jit_default_realloc_func(void *ptr, size_t size)
{
    char *p;

    if (!ptr)
	return jit_default_alloc_func(size);
    p = realloc((char *)ptr - JMC_HDR, size + JMC_HDR);
    if (!p)
	return NULL;
    *(size_t *)p = size;
    return p + JMC_HDR;
}

static void
jit_default_free_func(void *ptr)
{
    char *p = (char *)ptr - JMC_HDR;
    size_t size = *(size_t *)p;
    unsigned int i, empty = JMC_SIZES;

    if (size >= JMC_MIN) {
	jmc_acquire();
	for (i = 0; i < JMC_SIZES; i++) {
	    if (jmc[i].size == size) {
		if (jmc[i].n < JMC_PER) {
		    jmc[i].p[jmc[i].n++] = p;
		    p = NULL;
		}
		break;
	    }
	    if (!jmc[i].n && empty == JMC_SIZES)
		empty = i;
	}
	if (p && i == JMC_SIZES && empty < JMC_SIZES) {
	    jmc[empty].size = size;
	    jmc[empty].p[0] = p;
	    jmc[empty].n = 1;
	    p = NULL;
	}
	jmc_release();
    }
    if (p)
	free(p);
}

/* bytes dcbz clears (cache line), found once; 0: use memset */
static int jit_dcbz_size = -1;

static int
jit_dcbz_probe(void)
{
#  if defined(__powerpc__) || defined(__ppc__) || defined(__PPC__)
    static unsigned char buf[1024] __attribute__((aligned(256)));
    unsigned char *line = buf + 256;
    int n = 0;

    memset(buf, 0xff, sizeof(buf));
    __asm__ volatile("dcbz 0,%0" : : "r"(line) : "memory");
    while (n < 512 && line[n] == 0)
	n++;
    /* only a clean power-of-two line, nothing touched before it */
    if ((n == 32 || n == 64 || n == 128) && buf[255] == 0xff && line[n] == 0xff)
	return n;
#  endif
    return 0;
}

static void
jit_zero(void *ptr, size_t size)
{
    int line = jit_dcbz_size;

    if (line < 0)
	line = jit_dcbz_size = jit_dcbz_probe();
#  if defined(__powerpc__) || defined(__ppc__) || defined(__PPC__)
    if (line && size >= 4 * (size_t)line) {
	uintptr_t a = (uintptr_t)ptr, end = a + size;
	uintptr_t first = (a + line - 1) & ~(uintptr_t)(line - 1);
	uintptr_t last = end & ~(uintptr_t)(line - 1);

	memset(ptr, 0, first - a);
	for (; first < last; first += line)
	    __asm__ volatile("dcbz 0,%0" : : "r"(first) : "memory");
	memset((void *)last, 0, end - last);
	return;
    }
#  endif
    memset(ptr, 0, size);
}
#else
static void *
jit_default_alloc_func(size_t size)
{
    return (malloc(size));
}

static void *
jit_default_realloc_func(void *ptr, size_t size)
{
    return (realloc(ptr, size));
}

static void
jit_default_free_func(void *ptr)
{
    free(ptr);
}
#endif
