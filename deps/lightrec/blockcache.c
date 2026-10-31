// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Copyright (C) 2015-2021 Paul Cercueil <paul@crapouillou.net>
 */

#include "blockcache.h"
#include "debug.h"
#include "lightrec-private.h"
#include "memmanager.h"
#include "reaper.h"
#include "recompiler.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Must be power of two */
#define LUT_SIZE 0x4000

/* code cache: blocks retired because their code was overwritten, kept with
 * their compiled code, oldest first. A game that loads the same code again
 * (a racing game's race code after the menus) gets the blocks back without
 * compiling them: a compile costs about 0.3 to 2 ms per block on the PS3. */
#define STASH_BUCKETS	1024			/* power of two */
#define STASH_MAX	4096			/* blocks */
#define STASH_BYTES	(2 * 1024 * 1024)	/* compiled code kept */

struct blockcache {
	struct lightrec_state *state;
	struct block * lut[LUT_SIZE];
	struct block * stash[STASH_BUCKETS];	/* chained through block->next */
	struct block * stash_ring[STASH_MAX];	/* insertion order, NULL = taken */
	u32 stash_head, stash_used, stash_count;
	size_t stash_bytes;
};

static inline u32 stash_bucket(u32 pc)
{
	return (kunseg(pc) >> 2) & (STASH_BUCKETS - 1);
}

static void stash_unlink(struct blockcache *cache, struct block *block)
{
	struct block **p = &cache->stash[stash_bucket(block->pc)];

	for (; *p; p = &(*p)->next) {
		if (*p == block) {
			*p = block->next;
			break;
		}
	}

	block->next = NULL;
	cache->stash_ring[block->stash_slot] = NULL;
	cache->stash_count--;
	cache->stash_bytes -= block->code_size;
}

/* drops the oldest kept block (a slot left empty by a taken one is skipped) */
static bool stash_drop_oldest(struct blockcache *cache)
{
	struct block *block;

	while (cache->stash_used) {
		block = cache->stash_ring[cache->stash_head];
		cache->stash_ring[cache->stash_head] = NULL;
		cache->stash_head = (cache->stash_head + 1) % STASH_MAX;
		cache->stash_used--;

		if (block) {
			stash_unlink(cache, block);
			lightrec_free_block(cache->state, block);
			lightrec_dbg.n_stash_drop++;
			return true;
		}
	}

	return false;
}

/* (called on the emulation thread, with the block out of the cache and out of
   the compile queue) */
void lightrec_stash_block(struct blockcache *cache, struct block *block)
{
	u32 slot;

	if (!block->function || !block->code_copy ||
	    block_has_flag(block, BLOCK_SHOULD_RECOMPILE | BLOCK_NEVER_COMPILE) ||
	    block->code_size > STASH_BYTES / 4) {
		lightrec_free_block(cache->state, block);
		return;
	}

	while (cache->stash_used == STASH_MAX ||
	       (cache->stash_count &&
		cache->stash_bytes + block->code_size > STASH_BYTES)) {
		if (!stash_drop_oldest(cache))
			break;
	}

	slot = (cache->stash_head + cache->stash_used) % STASH_MAX;
	cache->stash_ring[slot] = block;
	cache->stash_used++;
	block->stash_slot = slot;

	block->next = cache->stash[stash_bucket(block->pc)];
	cache->stash[stash_bucket(block->pc)] = block;
	cache->stash_count++;
	cache->stash_bytes += block->code_size;
	lightrec_dbg.n_stash_add++;
}

/* a kept block for this address whose code is exactly what is there now */
struct block * lightrec_stash_take(struct blockcache *cache, u32 pc,
				   const u32 *code)
{
	struct block *block;

	for (block = cache->stash[stash_bucket(pc)]; block; block = block->next) {
		if (kunseg(block->pc) != kunseg(pc) || block->pc != pc)
			continue;
		if (memcmp(block->code_copy, code,
			   block->code_copy_len * sizeof(u32)))
			continue;

		stash_unlink(cache, block);
		lightrec_dbg.n_stash_hit++;
		return block;
	}

	return NULL;
}

bool lightrec_stash_clear(struct blockcache *cache)
{
	unsigned int i;
	struct block *block, *next;
	bool any = cache->stash_count != 0;

	for (i = 0; i < STASH_BUCKETS; i++) {
		for (block = cache->stash[i]; block; block = next) {
			next = block->next;
			lightrec_free_block(cache->state, block);
		}
		cache->stash[i] = NULL;
	}

	memset(cache->stash_ring, 0, sizeof(cache->stash_ring));
	cache->stash_head = cache->stash_used = cache->stash_count = 0;
	cache->stash_bytes = 0;

	return any;
}

u16 lightrec_get_lut_entry(const struct block *block)
{
	return (kunseg(block->pc) >> 2) & (LUT_SIZE - 1);
}

struct block * lightrec_find_block(struct blockcache *cache, u32 pc)
{
	struct block *block;

	pc = kunseg(pc);

	for (block = cache->lut[(pc >> 2) & (LUT_SIZE - 1)];
	     block; block = block->next)
		if (kunseg(block->pc) == pc)
			return block;

	return NULL;
}

struct block * lightrec_find_block_from_lut(struct blockcache *cache,
					    u16 lut_entry, u32 addr_in_block)
{
	struct block *block;
	u32 pc;

	addr_in_block = kunseg(addr_in_block);

	for (block = cache->lut[lut_entry]; block; block = block->next) {
		pc = kunseg(block->pc);
		if (addr_in_block >= pc &&
		    addr_in_block < pc + (block->nb_ops << 2))
			return block;
	}

	return NULL;
}

void remove_from_code_lut(struct blockcache *cache, struct block *block)
{
	struct lightrec_state *state = cache->state;
	u32 offset = lut_offset(block->pc);

	if (block->function) {
		memset(lut_address(state, offset), 0,
		       block->nb_ops * lut_elm_size(state));
	}
}

void lightrec_register_block(struct blockcache *cache, struct block *block)
{
	u32 pc = kunseg(block->pc);
	struct block *old;

	old = cache->lut[(pc >> 2) & (LUT_SIZE - 1)];
	if (old)
		block->next = old;

	cache->lut[(pc >> 2) & (LUT_SIZE - 1)] = block;

	remove_from_code_lut(cache, block);
}

/* (a block taken back from the code cache: its compiled code exists, and
   entries of other blocks inside its range stay) */
void lightrec_register_block_keep_lut(struct blockcache *cache,
				      struct block *block)
{
	u32 pc = kunseg(block->pc);
	struct block *old;

	old = cache->lut[(pc >> 2) & (LUT_SIZE - 1)];
	block->next = old;
	cache->lut[(pc >> 2) & (LUT_SIZE - 1)] = block;
}

void lightrec_unregister_block(struct blockcache *cache, struct block *block)
{
	u32 pc = kunseg(block->pc);
	struct block *old = cache->lut[(pc >> 2) & (LUT_SIZE - 1)];

	if (old == block) {
		cache->lut[(pc >> 2) & (LUT_SIZE - 1)] = old->next;
		return;
	}

	for (; old; old = old->next) {
		if (old->next == block) {
			old->next = block->next;
			return;
		}
	}

	pr_err("Block at "PC_FMT" is not in cache\n", block->pc);
}

static bool lightrec_block_is_old(const struct lightrec_state *state,
				  const struct block *block)
{
	u32 diff = state->current_cycle - block->precompile_date;

	return diff > (1 << 27); /* About 4 seconds */
}

static void lightrec_free_blocks(struct blockcache *cache,
				 const struct block *except, bool all)
{
	struct lightrec_state *state = cache->state;
	struct block *block, *next;
	bool outdated = all;
	unsigned int i;
	u8 old_flags;

	for (i = 0; i < LUT_SIZE; i++) {
		for (block = cache->lut[i]; block; block = next) {
			next = block->next;

			if (except && block == except)
				continue;

			if (!all) {
				outdated = lightrec_block_is_old(state, block) ||
					lightrec_block_is_outdated(state, block);
			}

			if (!outdated)
				continue;

			old_flags = block_set_flags(block, BLOCK_IS_DEAD);

			if (!(old_flags & BLOCK_IS_DEAD)) {
				if (ENABLE_THREADED_COMPILER)
					lightrec_recompiler_remove(state->rec, block);

				pr_debug("Freeing outdated block at "PC_FMT"\n", block->pc);
				remove_from_code_lut(cache, block);
				lightrec_unregister_block(cache, block);
				lightrec_free_block(state, block);
			}
		}
	}
}

void lightrec_remove_outdated_blocks(struct blockcache *cache,
				     const struct block *except)
{
	pr_info("Running out of code space. Cleaning block cache...\n");

	lightrec_free_blocks(cache, except, false);
}

void lightrec_free_all_blocks_except(struct blockcache *cache,
				     const struct block *except)
{
	lightrec_stash_clear(cache);
	lightrec_free_blocks(cache, except, true);
}

void lightrec_free_all_blocks(struct blockcache *cache)
{
	lightrec_free_blocks(cache, NULL, true);
}

void lightrec_free_block_cache(struct blockcache *cache)
{
	lightrec_stash_clear(cache);
	lightrec_free_all_blocks(cache);
	lightrec_free(cache->state, MEM_FOR_LIGHTREC, sizeof(*cache), cache);
}

struct blockcache * lightrec_blockcache_init(struct lightrec_state *state)
{
	struct blockcache *cache;

	cache = lightrec_calloc(state, MEM_FOR_LIGHTREC, sizeof(*cache));
	if (!cache)
		return NULL;

	cache->state = state;

	return cache;
}

u32 lightrec_calculate_block_hash(const struct block *block)
{
	const u32 *code = block->code;
	u32 hash = 0xffffffff;
	unsigned int i;

	/* Jenkins one-at-a-time hash algorithm */
	for (i = 0; i < block->nb_ops; i++) {
		hash += *code++;
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}

	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);

	return hash;
}

static void lightrec_reset_lut_offset(struct lightrec_state *state, void *d)
{
	u32 pc = (u32)(uintptr_t) d;
	struct block *block;
	void *addr;

	block = lightrec_find_block(state->block_cache, pc);
	if (!block)
		return;

	if (block_has_flag(block, BLOCK_IS_DEAD))
		return;

	addr = block->function ?: state->get_next_block;
	lut_write(state, lut_offset(pc), addr);
}

bool lightrec_block_is_outdated(struct lightrec_state *state, struct block *block)
{
	u32 offset = lut_offset(block->pc);
	bool outdated;

	if (lut_read(state, offset))
		return false;

	outdated = block->hash != lightrec_calculate_block_hash(block);
	if (likely(!outdated)) {
		/* The block was marked as outdated, but the content is still
		 * the same */

		if (ENABLE_THREADED_COMPILER) {
			/*
			 * When compiling a block that covers ours, the threaded
			 * compiler will set the LUT entries of the various
			 * entry points. Therefore we cannot write the LUT here,
			 * as we would risk overwriting the new entry points.
			 * Leave it to the reaper to re-install the LUT entries.
			 */

			lightrec_reaper_add(state->reaper,
					    lightrec_reset_lut_offset,
					    (void *)(uintptr_t) block->pc);
		} else if (block->function) {
			lut_write(state, offset, block->function);
		} else {
			lut_write(state, offset, state->get_next_block);
		}
	}

	return outdated;
}
