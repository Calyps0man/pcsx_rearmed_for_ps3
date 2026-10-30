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
#include <lightning/jit_private.h>

/*
 * Prototypes
 */
static void *jit_default_alloc_func(size_t);
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
    memset(*ptr, 0, size);
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
	memset((jit_int8_t*)*ptr + old_size, 0, new_size - old_size);
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
