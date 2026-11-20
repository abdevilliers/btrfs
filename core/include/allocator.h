/*
 * allocator.h - Allocator: abstract data type for free space and
 * reference counts.
 *
 * Every block has a reference count = how many things point to it (a
 * parent node, a checkpoint, the live volume). A block whose count
 * drops to 0 is free again (once no commit needs it: see alloc_commit).
 */
#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stdint.h>

typedef struct Allocator Allocator;

Allocator *alloc_create(uint32_t nblocks);
void alloc_destroy(Allocator *a);

uint32_t alloc_block(Allocator *a);               /* count becomes 1; 0 if full */
void alloc_ref(Allocator *a, uint32_t blk);       /* count + 1 */
uint32_t alloc_unref(Allocator *a, uint32_t blk); /* count - 1, returns new count */
uint32_t alloc_refcount(const Allocator *a, uint32_t blk);

uint32_t alloc_used(const Allocator *a);          /* blocks in use */
uint32_t alloc_free(const Allocator *a);          /* blocks that can be handed out */

/* Commits (used by recovery). alloc_commit remembers which blocks are in
 * use right now. Until the next commit those blocks are never handed out
 * again, even if their count drops to 0, so the committed state stays
 * readable on disk whatever happens afterwards. */
void alloc_commit(Allocator *a);
int alloc_committed(const Allocator *a, uint32_t blk); /* in use at the last commit? */

/* Set a count directly: for reserving blocks and for loading saved state. */
void alloc_set(Allocator *a, uint32_t blk, uint32_t count);

#endif
