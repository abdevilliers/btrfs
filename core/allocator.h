/* allocator.h - free space + a reference count per block
 * (btrfs counterpart: the extent tree).
 *
 * refs[b]      = how many parents point at block b right now.
 * committed[b] = refs[b] as of the last commit (used by recovery).
 *
 * A block is free only when BOTH are 0: a block the last commit still
 * uses is never handed out again until the next commit.
 */
#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stdint.h>

typedef struct Allocator Allocator;   /* opaque */

Allocator *alloc_create(uint32_t nblocks, uint32_t first_usable);
void       alloc_destroy(Allocator *a);

uint32_t alloc_block(Allocator *a);              /* new block, refs = 1 */
void     alloc_ref(Allocator *a, uint32_t b);    /* refs + 1 */
uint32_t alloc_unref(Allocator *a, uint32_t b);  /* refs - 1, returns new count */
uint32_t alloc_refs(const Allocator *a, uint32_t b);
int      alloc_is_committed(const Allocator *a, uint32_t b);

void     alloc_commit(Allocator *a);                  /* committed = refs */
const uint32_t *alloc_table(const Allocator *a);      /* refs[], for saving */
void     alloc_load(Allocator *a, const uint32_t *t); /* refs = committed = t */
uint32_t alloc_used(const Allocator *a);              /* blocks not free */
uint32_t alloc_nblocks(const Allocator *a);
uint32_t alloc_first(const Allocator *a);

#endif
