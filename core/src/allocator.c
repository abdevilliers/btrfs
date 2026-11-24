/*
 * allocator.c - one reference count per block, plus a mark for the blocks
 * the last commit uses.
 */
#include <assert.h>
#include <stdlib.h>

#include "allocator.h"

struct Allocator {
    uint32_t nblocks;
    uint32_t *refs;     /* refs[b] = reference count of block b        */
    uint8_t *committed; /* committed[b] = 1 if the last commit uses b  */
};

/* A block can be handed out when nothing points to it now AND the last
 * commit does not use it. */
static int is_free(const Allocator *a, uint32_t b)
{
    return a->refs[b] == 0 && !a->committed[b];
}

Allocator *alloc_create(uint32_t nblocks)
{
    Allocator *a = malloc(sizeof *a);
    if (!a)
        return NULL;
    a->refs = calloc(nblocks, sizeof *a->refs);
    a->committed = calloc(nblocks, 1);
    if (!a->refs || !a->committed) {
        free(a->refs);
        free(a->committed);
        free(a);
        return NULL;
    }
    a->nblocks = nblocks;
    a->refs[0] = 1; /* block 0 means "no block": never handed out */
    return a;
}

void alloc_destroy(Allocator *a)
{
    if (a) {
        free(a->refs);
        free(a->committed);
        free(a);
    }
}

uint32_t alloc_block(Allocator *a)
{
    for (uint32_t b = 1; b < a->nblocks; b++)
        if (is_free(a, b)) {
            a->refs[b] = 1;
            return b;
        }
    return 0;
}

void alloc_ref(Allocator *a, uint32_t blk)
{
    assert(blk > 0 && blk < a->nblocks && a->refs[blk] > 0);
    a->refs[blk]++;
}

uint32_t alloc_unref(Allocator *a, uint32_t blk)
{
    assert(blk > 0 && blk < a->nblocks && a->refs[blk] > 0);
    return --a->refs[blk];
}

uint32_t alloc_refcount(const Allocator *a, uint32_t blk)
{
    return blk < a->nblocks ? a->refs[blk] : 0;
}

uint32_t alloc_used(const Allocator *a)
{
    uint32_t used = 0;
    for (uint32_t b = 1; b < a->nblocks; b++)
        if (a->refs[b])
            used++;
    return used;
}

uint32_t alloc_free(const Allocator *a)
{
    uint32_t n = 0;
    for (uint32_t b = 1; b < a->nblocks; b++)
        if (is_free(a, b))
            n++;
    return n;
}

void alloc_commit(Allocator *a)
{
    for (uint32_t b = 0; b < a->nblocks; b++)
        a->committed[b] = a->refs[b] != 0;
}

int alloc_committed(const Allocator *a, uint32_t blk)
{
    return blk < a->nblocks && a->committed[blk];
}

void alloc_set(Allocator *a, uint32_t blk, uint32_t count)
{
    assert(blk > 0 && blk < a->nblocks);
    a->refs[blk] = count;
}
