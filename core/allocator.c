#include "allocator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Allocator {
    uint32_t n, first;
    uint32_t *refs;
    uint32_t *committed;
};

Allocator *alloc_create(uint32_t n, uint32_t first)
{
    Allocator *a = calloc(1, sizeof *a);
    a->n = n;
    a->first = first;
    a->refs = calloc(n, sizeof(uint32_t));
    a->committed = calloc(n, sizeof(uint32_t));
    return a;
}

void alloc_destroy(Allocator *a)
{
    if (!a) return;
    free(a->refs);
    free(a->committed);
    free(a);
}

uint32_t alloc_block(Allocator *a)
{
    for (uint32_t b = a->first; b < a->n; b++) {
        if (a->refs[b] == 0 && a->committed[b] == 0) {
            a->refs[b] = 1;
            return b;
        }
    }
    fprintf(stderr, "allocator: disk full (try commit, or delete checkpoints)\n");
    exit(1);
}

void alloc_ref(Allocator *a, uint32_t b) { a->refs[b]++; }

uint32_t alloc_unref(Allocator *a, uint32_t b)
{
    if (a->refs[b] == 0) {
        fprintf(stderr, "allocator: unref of free block %u\n", b);
        abort();
    }
    return --a->refs[b];
}

uint32_t alloc_refs(const Allocator *a, uint32_t b)        { return a->refs[b]; }
int      alloc_is_committed(const Allocator *a, uint32_t b) { return a->committed[b] != 0; }
void     alloc_commit(Allocator *a) { memcpy(a->committed, a->refs, a->n * sizeof(uint32_t)); }
const uint32_t *alloc_table(const Allocator *a) { return a->refs; }
uint32_t alloc_nblocks(const Allocator *a) { return a->n; }
uint32_t alloc_first(const Allocator *a)   { return a->first; }

void alloc_load(Allocator *a, const uint32_t *t)
{
    memcpy(a->refs, t, a->n * sizeof(uint32_t));
    memcpy(a->committed, t, a->n * sizeof(uint32_t));
}

uint32_t alloc_used(const Allocator *a)
{
    uint32_t used = 0;
    for (uint32_t b = a->first; b < a->n; b++)
        if (a->refs[b] || a->committed[b]) used++;
    return used;
}
