/*
 * checkpoint.c - create, read, rollback and delete checkpoints; go back x
 * checkpoints; take checkpoints at fixed times.
 *
 * Every function here only moves root pointers; the reference counting
 * and copy-on-write happen in core. None of them copies a page or a node.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkpoint.h"

/* ------------------------------------------------------------ by name */

int ckpt_create(Volume *v, const char *name)
{
    /* A new root-table entry pointing at the live tree; the live root
     * gains one reference. That is the whole checkpoint. */
    return vol_root_add(v, name, vol_live_root(v));
}

int ckpt_read(Volume *v, const char *name, uint32_t page, char *buf, size_t len)
{
    uint32_t root;
    int r = vol_root_find(v, name, &root);
    return r ? r : vol_read_tree(v, root, page, buf, len);
}

int ckpt_rollback(Volume *v, const char *name)
{
    uint32_t root;
    int r = vol_root_find(v, name, &root);
    if (r)
        return r;
    /* The live volume takes the checkpoint's tree and lets go of its own;
     * blocks that only the old live tree used are freed. */
    vol_set_live_root(v, root);
    return 0;
}

int ckpt_delete(Volume *v, const char *name)
{
    /* Drop the entry's reference: frees exactly what only it used. */
    return vol_root_remove(v, name);
}

/* -------------------------------------------------------- by position */

int ckpt_rollback_back(Volume *v, uint32_t x)
{
    char name[CKPT_NAME_MAX + 1];
    uint32_t root;
    /* The root table remembers the order entries were added in, so the
     * x-th newest checkpoint is one lookup. The rest is a normal rollback. */
    int r = vol_root_nth(v, x, name, &root);
    if (r)
        return r;
    vol_set_live_root(v, root);
    return 0;
}

int ckpt_name_back(Volume *v, uint32_t x, char *name)
{
    uint32_t root;
    return vol_root_nth(v, x, name, &root);
}

uint32_t ckpt_count(Volume *v)
{
    char name[CKPT_NAME_MAX + 1];
    uint32_t n = 0;
    while (ckpt_name_back(v, n + 1, name) == 0)
        n++;
    return n;
}

/* ----------------------------------------------------- at fixed times */

#define AUTO_PREFIX "auto-"

struct CkptTimer {
    Volume *vol;
    uint64_t interval; /* time between automatic checkpoints */
    uint64_t next;     /* the next fixed time one is due     */
    uint32_t keep;     /* automatic checkpoints to keep      */
};

CkptTimer *ckpt_timer_create(Volume *v, uint64_t interval, uint32_t keep, uint64_t now)
{
    CkptTimer *t = interval && keep ? malloc(sizeof *t) : NULL;
    if (!t)
        return NULL;
    t->vol = v;
    t->interval = interval;
    t->keep = keep;
    t->next = (now / interval + 1) * interval; /* first fixed time after now */
    return t;
}

void ckpt_timer_destroy(CkptTimer *t)
{
    free(t);
}

uint64_t ckpt_timer_next(const CkptTimer *t)
{
    return t->next;
}

/* Delete the oldest automatic checkpoints until at most `keep` are left. */
static void delete_old(Volume *v, uint32_t keep)
{
    char name[CKPT_NAME_MAX + 1];
    uint32_t autos = 0;
    uint32_t x = 1;

    while (ckpt_name_back(v, x, name) == 0) { /* newest first */
        int is_auto = strncmp(name, AUTO_PREFIX, strlen(AUTO_PREFIX)) == 0;
        if (is_auto && ++autos > keep)
            ckpt_delete(v, name); /* position x now holds the next older one */
        else
            x++;
    }
}

int ckpt_timer_tick(CkptTimer *t, uint64_t now)
{
    char name[CKPT_NAME_MAX + 1];

    if (now < t->next)
        return 0; /* not due yet */

    /* The fixed time this checkpoint belongs to: the latest multiple of
     * the interval that has passed. If several were missed (the caller
     * did not tick for a while), one checkpoint covers them. */
    uint64_t due = now / t->interval * t->interval;
    t->next = due + t->interval;

    delete_old(t->vol, t->keep - 1); /* make room for the new one */
    snprintf(name, sizeof name, AUTO_PREFIX "%llu", (unsigned long long)due);
    int r = ckpt_create(t->vol, name);
    if (r == -EEXIST)
        return 0; /* this fixed time already has its checkpoint */
    return r ? r : 1;
}
