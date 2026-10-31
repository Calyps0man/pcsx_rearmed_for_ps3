/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright (C) 2014-2021 Paul Cercueil <paul@crapouillou.net>
 */

#ifndef __BLOCKCACHE_H__
#define __BLOCKCACHE_H__

#include "lightrec.h"
#include <stdbool.h>

struct blockcache;

struct block * lightrec_find_block(struct blockcache *cache, u32 pc);
struct block * lightrec_find_block_from_lut(struct blockcache *cache,
					    u16 lut_entry, u32 addr_in_block);
u16 lightrec_get_lut_entry(const struct block *block);

void lightrec_register_block(struct blockcache *cache, struct block *block);
void lightrec_unregister_block(struct blockcache *cache, struct block *block);

struct blockcache * lightrec_blockcache_init(struct lightrec_state *state);
void lightrec_free_block_cache(struct blockcache *cache);

void lightrec_free_all_blocks(struct blockcache *cache);

/* code cache (see struct block) */
void lightrec_stash_block(struct blockcache *cache, struct block *block);
struct block * lightrec_stash_take(struct blockcache *cache, u32 pc,
				   const u32 *code);
bool lightrec_stash_clear(struct blockcache *cache);
void lightrec_register_block_keep_lut(struct blockcache *cache,
				      struct block *block);
void lightrec_free_all_blocks_except(struct blockcache *cache,
				     const struct block *except);

u32 lightrec_calculate_block_hash(const struct block *block);
_Bool lightrec_block_is_outdated(struct lightrec_state *state, struct block *block);

void lightrec_remove_outdated_blocks(struct blockcache *cache,
				     const struct block *except);

#endif /* __BLOCKCACHE_H__ */
