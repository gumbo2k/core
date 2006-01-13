/* Copyright (c) 2002-2003 Timo Sirainen */

/* @UNSAFE: whole file */
#include "lib.h"
#include "mempool.h"

#include <stdlib.h>

#ifdef HAVE_GC_GC_H
#  include <gc/gc.h>
#elif defined (HAVE_GC_H)
#  include <gc.h>
#endif

#define MAX_ALLOC_SIZE SSIZE_T_MAX

struct alloconly_pool {
	struct pool pool;
	int refcount;

	size_t base_size;
	struct pool_block *block;
#ifdef DEBUG
	const char *name;
#endif
};

struct pool_block {
	struct pool_block *prev;

	size_t size;
	size_t left;
	size_t last_alloc_size;

	/* unsigned char data[]; */
};
#define SIZEOF_POOLBLOCK (MEM_ALIGN(sizeof(struct pool_block)))

#define POOL_BLOCK_DATA(block) \
	((char *) (block) + SIZEOF_POOLBLOCK)

static const char *pool_alloconly_get_name(pool_t pool);
static void pool_alloconly_ref(pool_t pool);
static void pool_alloconly_unref(pool_t pool);
static void *pool_alloconly_malloc(pool_t pool, size_t size);
static void pool_alloconly_free(pool_t pool, void *mem);
static void *pool_alloconly_realloc(pool_t pool, void *mem,
				    size_t old_size, size_t new_size);
static void pool_alloconly_clear(pool_t pool);
static size_t pool_alloconly_get_max_easy_alloc_size(pool_t pool);

static void block_alloc(struct alloconly_pool *pool, size_t size);

static struct pool static_alloconly_pool = {
	pool_alloconly_get_name,

	pool_alloconly_ref,
	pool_alloconly_unref,

	pool_alloconly_malloc,
	pool_alloconly_free,

	pool_alloconly_realloc,

	pool_alloconly_clear,
	pool_alloconly_get_max_easy_alloc_size,

	TRUE,
	FALSE
};

pool_t pool_alloconly_create(const char *name __attr_unused__, size_t size)
{
	struct alloconly_pool apool, *new_apool;
	size_t min_alloc = sizeof(struct alloconly_pool) + SIZEOF_POOLBLOCK;

#ifdef DEBUG
	min_alloc += strlen(name) + 1;
#endif

	/* create a fake alloconly_pool so we can call block_alloc() */
	memset(&apool, 0, sizeof(apool));
	apool.pool = static_alloconly_pool;
	apool.refcount = 1;

	if (size < min_alloc)
		size = nearest_power(size + min_alloc);
	block_alloc(&apool, size);

	/* now allocate the actual alloconly_pool from the created block */
	new_apool = p_new(&apool.pool, struct alloconly_pool, 1);
	*new_apool = apool;
#ifdef DEBUG
	new_apool->name = p_strdup(&new_apool->pool, name);
#endif

	/* set base_size so p_clear() doesn't trash alloconly_pool structure. */
	new_apool->base_size = new_apool->block->size - new_apool->block->left;
	new_apool->block->last_alloc_size = 0;

	return &new_apool->pool;
}

static void pool_alloconly_destroy(struct alloconly_pool *apool)
{
	void *block;

	/* destroy all but the last block */
	pool_alloconly_clear(&apool->pool);

	/* destroy the last block */
	block = apool->block;
#ifdef DEBUG
	memset(block, 0xde, SIZEOF_POOLBLOCK + apool->block->size);
#endif

#ifndef USE_GC
	free(block);
#endif
}

static const char *pool_alloconly_get_name(pool_t pool __attr_unused__)
{
#ifdef DEBUG
	struct alloconly_pool *apool = (struct alloconly_pool *) pool;

	return apool->name;
#else
	return "alloconly";
#endif
}

static void pool_alloconly_ref(pool_t pool)
{
	struct alloconly_pool *apool = (struct alloconly_pool *) pool;

	apool->refcount++;
}

static void pool_alloconly_unref(pool_t pool)
{
	struct alloconly_pool *apool = (struct alloconly_pool *) pool;

	if (--apool->refcount == 0)
		pool_alloconly_destroy(apool);
}

static void block_alloc(struct alloconly_pool *apool, size_t size)
{
	struct pool_block *block;

	i_assert(size > SIZEOF_POOLBLOCK);

	if (apool->block != NULL) {
		/* each block is at least twice the size of the previous one */
		if (size <= apool->block->size)
			size += apool->block->size;

		size = nearest_power(size);
#ifdef DEBUG
		i_warning("Growing pool '%s' with: %"PRIuSIZE_T,
			  apool->name, size);
#endif
	}

#ifndef USE_GC
	block = calloc(size, 1);
#else
	block = GC_malloc(size);
	memset(block, 0, size);
#endif
	if (block == NULL)
		i_fatal_status(FATAL_OUTOFMEM, "block_alloc(): Out of memory");
	block->prev = apool->block;
	apool->block = block;

	block->size = size - SIZEOF_POOLBLOCK;
	block->left = block->size;
}

static void *pool_alloconly_malloc(pool_t pool, size_t size)
{
	struct alloconly_pool *apool = (struct alloconly_pool *) pool;
	void *mem;

	if (size == 0 || size > SSIZE_T_MAX)
		i_panic("Trying to allocate %"PRIuSIZE_T" bytes", size);

	size = MEM_ALIGN(size);

	if (apool->block->left < size) {
		/* we need a new block */
		block_alloc(apool, size + SIZEOF_POOLBLOCK);
	}

	mem = POOL_BLOCK_DATA(apool->block) +
		(apool->block->size - apool->block->left);

	apool->block->left -= size;
	apool->block->last_alloc_size = size;
	return mem;
}

static void pool_alloconly_free(pool_t pool, void *mem)
{
	struct alloconly_pool *apool = (struct alloconly_pool *) pool;

	/* we can free only the last allocation */
	if (POOL_BLOCK_DATA(apool->block) +
	    (apool->block->size - apool->block->left -
	     apool->block->last_alloc_size) == mem) {
		memset(mem, 0, apool->block->last_alloc_size);
		apool->block->left += apool->block->last_alloc_size;
                apool->block->last_alloc_size = 0;
	}
}

static bool pool_try_grow(struct alloconly_pool *apool, void *mem, size_t size)
{
	/* see if we want to grow the memory we allocated last */
	if (POOL_BLOCK_DATA(apool->block) +
	    (apool->block->size - apool->block->left -
	     apool->block->last_alloc_size) == mem) {
		/* yeah, see if we can grow */
		if (apool->block->left >= size-apool->block->last_alloc_size) {
			/* just shrink the available size */
			apool->block->left -=
				size - apool->block->last_alloc_size;
			apool->block->last_alloc_size = size;
			return TRUE;
		}
	}

	return FALSE;
}

static void *pool_alloconly_realloc(pool_t pool, void *mem,
				    size_t old_size, size_t new_size)
{
	struct alloconly_pool *apool = (struct alloconly_pool *) pool;
	unsigned char *new_mem;

	if (new_size == 0 || new_size > SSIZE_T_MAX)
		i_panic("Trying to allocate %"PRIuSIZE_T" bytes", new_size);

	if (mem == NULL)
		return pool_alloconly_malloc(pool, new_size);

	if (new_size <= old_size)
		return mem;

	new_size = MEM_ALIGN(new_size);

	/* see if we can directly grow it */
	if (!pool_try_grow(apool, mem, new_size)) {
		/* slow way - allocate + copy */
		new_mem = pool_alloconly_malloc(pool, new_size);
		memcpy(new_mem, mem, old_size);
		mem = new_mem;
	}

        return mem;
}

static void pool_alloconly_clear(pool_t pool)
{
	struct alloconly_pool *apool = (struct alloconly_pool *) pool;
	struct pool_block *block;
	size_t avail_size;

	/* destroy all blocks but the oldest, which contains the
	   struct alloconly_pool allocation. */
	while (apool->block->prev != NULL) {
		block = apool->block;
		apool->block = block->prev;

#ifdef DEBUG
		memset(block, 0xde, SIZEOF_POOLBLOCK + block->size);
#endif
#ifndef USE_GC
		free(block);
#endif
	}

	/* clear the first block */
	avail_size = apool->block->size - apool->base_size;
	memset(PTR_OFFSET(POOL_BLOCK_DATA(apool->block), apool->base_size), 0,
	       avail_size - apool->block->left);
	apool->block->left = avail_size;
	apool->block->last_alloc_size = 0;
}

static size_t pool_alloconly_get_max_easy_alloc_size(pool_t pool)
{
	struct alloconly_pool *apool = (struct alloconly_pool *) pool;

	return apool->block->left;
}
