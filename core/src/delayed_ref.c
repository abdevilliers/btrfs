/*
 * delayed_ref.c - reference count changes, queued (fs/btrfs/delayed-ref.c).
 *
 * Changing a tree never touches the extent tree directly: every "one more
 * reference to block X" or "one less" is queued here, in a HEAD per
 * bytenr. Changes to the same reference cancel out (+1 then -1 = nothing).
 * The commit runs the queue (run_delayed_refs): only then are extent items
 * and back-references inserted, changed or deleted. Running them changes
 * the extent tree, which queues more changes for its own blocks; the loop
 * runs until the queue is empty.
 */
#include <errno.h>
#include <stdlib.h>

#include "ctree.h"

static size_t bucket(const DelayedRefs *d, uint64_t bytenr)
{
    return (size_t)((bytenr >> 12) * 2654435761u) & (d->nbuckets - 1);
}

static int grow(DelayedRefs *d)
{
    size_t n = d->nbuckets ? d->nbuckets * 2 : 256;
    DelayedHead **nb = calloc(n, sizeof *nb);
    if (!nb)
        return -ENOMEM;
    for (size_t i = 0; i < d->nbuckets; i++)
        for (DelayedHead *h = d->buckets[i], *next; h; h = next) {
            next = h->hnext;
            size_t k = (size_t)((h->bytenr >> 12) * 2654435761u) & (n - 1);
            h->hnext = nb[k];
            nb[k] = h;
        }
    free(d->buckets);
    d->buckets = nb;
    d->nbuckets = n;
    return 0;
}

DelayedHead *find_delayed_head(Btrfs *b, uint64_t bytenr)
{
    DelayedRefs *d = &b->delayed;
    if (!d->nbuckets)
        return NULL;
    for (DelayedHead *h = d->buckets[bucket(d, bytenr)]; h; h = h->hnext)
        if (h->bytenr == bytenr)
            return h;
    return NULL;
}

static DelayedHead *get_head(Btrfs *b, uint64_t bytenr, uint64_t num_bytes, int is_data, int level)
{
    DelayedRefs *d = &b->delayed;
    DelayedHead *h = find_delayed_head(b, bytenr);
    if (h)
        return h;
    if (d->count + 1 > d->nbuckets && grow(d) < 0)
        return NULL;
    h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->bytenr = bytenr;
    h->num_bytes = num_bytes;
    h->is_data = is_data;
    h->level = level;
    size_t k = bucket(d, bytenr);
    h->hnext = d->buckets[k];
    d->buckets[k] = h;
    d->count++;
    return h;
}

void remove_delayed_head(Btrfs *b, DelayedHead *h)
{
    DelayedRefs *d = &b->delayed;
    DelayedHead **pp = &d->buckets[bucket(d, h->bytenr)];
    while (*pp && *pp != h)
        pp = &(*pp)->hnext;
    if (*pp) {
        *pp = h->hnext;
        d->count--;
    }
    h->hnext = NULL;
}

void free_delayed_head(DelayedHead *h)
{
    if (h) {
        free(h->refs);
        free(h);
    }
}

static int same_ref(const DelayedRef *a, const DelayedRef *b)
{
    return a->type == b->type && a->root == b->root && a->parent == b->parent &&
           a->owner == b->owner && a->offset == b->offset;
}

int add_delayed_ref(Btrfs *b, uint64_t bytenr, uint64_t num_bytes, int is_data, int level,
                    const DelayedRef *ref, int must_insert)
{
    DelayedHead *h = get_head(b, bytenr, num_bytes, is_data, level);
    if (!h)
        return -ENOMEM;
    if (must_insert)
        h->must_insert = 1;
    h->ref_mod += ref->count;
    for (size_t i = 0; i < h->nrefs; i++)
        if (same_ref(&h->refs[i], ref)) {
            h->refs[i].count += ref->count;
            if (h->refs[i].count == 0)
                h->refs[i] = h->refs[--h->nrefs]; /* +1 and -1 cancel */
            return 0;
        }
    if (h->nrefs == h->cap) {
        size_t nc = h->cap ? h->cap * 2 : 4;
        DelayedRef *nr = realloc(h->refs, nc * sizeof *nr);
        if (!nr)
            return -ENOMEM;
        h->refs = nr;
        h->cap = nc;
    }
    h->refs[h->nrefs++] = *ref;
    b->trans_dirty = 1;
    return 0;
}

int add_delayed_extent_op(Btrfs *b, uint64_t bytenr, uint64_t num_bytes, int is_data, int level,
                          uint64_t flags)
{
    DelayedHead *h = get_head(b, bytenr, num_bytes, is_data, level);
    if (!h)
        return -ENOMEM;
    h->set_flags |= flags;
    b->trans_dirty = 1;
    return 0;
}

static int cmp_heads(const void *a, const void *b)
{
    const DelayedHead *x = *(DelayedHead *const *)a, *y = *(DelayedHead *const *)b;
    return x->bytenr < y->bytenr ? -1 : x->bytenr > y->bytenr;
}

/* btrfs_run_delayed_refs: until nothing is queued, take every head (in
 * bytenr order) and apply it to the extent tree. */
int run_delayed_refs(Trans *t)
{
    Btrfs *b = t->b;
    DelayedRefs *d = &b->delayed;

    while (d->count) {
        size_t n = 0, cap = d->count;
        DelayedHead **list = malloc(cap * sizeof *list);
        if (!list)
            return -ENOMEM;
        for (size_t i = 0; i < d->nbuckets; i++)
            for (DelayedHead *h = d->buckets[i]; h; h = h->hnext)
                list[n++] = h;
        for (size_t i = 0; i < n; i++)
            remove_delayed_head(b, list[i]);
        qsort(list, n, sizeof *list, cmp_heads);
        int r = 0;
        for (size_t i = 0; i < n; i++) {
            if (r == 0)
                r = run_one_head(t, list[i]);
            free_delayed_head(list[i]);
        }
        free(list);
        if (r < 0)
            return trans_abort(t, r);
    }
    return 0;
}

void delayed_refs_destroy(Btrfs *b)
{
    DelayedRefs *d = &b->delayed;
    for (size_t i = 0; i < d->nbuckets; i++)
        for (DelayedHead *h = d->buckets[i], *next; h; h = next) {
            next = h->hnext;
            free_delayed_head(h);
        }
    free(d->buckets);
    memset(d, 0, sizeof *d);
}
