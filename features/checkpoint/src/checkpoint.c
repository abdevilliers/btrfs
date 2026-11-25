/*
 * checkpoint.c - checkpoints as read-only snapshots, rollback as a
 * writable snapshot made the default subvolume. Nothing here copies data
 * or walks a tree: the core's snapshot (one copied root block) and
 * copy-on-write do it all.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkpoint.h"

static Fs *top(Btrfs *b)
{
    return fs_open_subvol(b, BTRFS_FS_TREE_OBJECTID);
}

static int valid_name(const char *name)
{
    return name && *name && strlen(name) <= CKPT_NAME_MAX && !strchr(name, '/') && strcmp(name, ".") &&
           strcmp(name, "..");
}

static void ckpt_path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", CKPT_DIR, name);
}

int ckpt_setup(Btrfs *b)
{
    Fs *t = top(b);
    FsStat st;
    uint64_t id;
    int r;
    if (!t)
        return -EIO;
    if (fs_stat(t, CKPT_DIR, &st) == -ENOENT && (r = fs_mkdir(t, CKPT_DIR)) < 0)
        goto out;
    r = 0;
    if (fs_get_default_subvol(b) == BTRFS_FS_TREE_OBJECTID) {
        if (fs_stat(t, "/@", &st) == -ENOENT && (r = fs_subvol_create(t, "/@")) < 0)
            goto out;
        if ((r = fs_subvol_lookup(t, "/@", &id)) == 0)
            r = fs_set_default_subvol(b, id);
    }
    if (r == 0)
        r = btrfs_commit(b);
out:
    fs_close(t);
    return r;
}

/* The live subvolume's path in the top level ("/@", "/@1", ...). */
static int live_path(Btrfs *b, char *out, size_t n)
{
    FsSubvolInfo info;
    uint64_t live = fs_get_default_subvol(b);
    if (live == BTRFS_FS_TREE_OBJECTID)
        return -EINVAL; /* ckpt_setup was not run */
    int r = fs_subvol_info(b, live, &info);
    if (r < 0)
        return r;
    if (info.parent_id != BTRFS_FS_TREE_OBJECTID || info.dirid != FS_ROOT_INO)
        return -EINVAL;
    snprintf(out, n, "/%s", info.name);
    return 0;
}

int ckpt_create(Btrfs *b, const char *name)
{
    char src[FS_NAME_MAX + 2], dst[FS_NAME_MAX + 32];
    if (!valid_name(name))
        return -EINVAL;
    int r = live_path(b, src, sizeof src);
    if (r < 0)
        return r;
    Fs *t = top(b);
    if (!t)
        return -EIO;
    ckpt_path(dst, sizeof dst, name);
    r = fs_subvol_snapshot(t, src, dst, 1, NULL);
    fs_close(t);
    return r;
}

/* ---------------------------------------------- the list, newest first */

typedef struct {
    uint64_t dir_ino;
    FsSubvolInfo *list;
    size_t n, cap;
} List;

static void collect(void *arg, const FsSubvolInfo *info)
{
    List *l = arg;
    if (info->parent_id != BTRFS_FS_TREE_OBJECTID || info->dirid != l->dir_ino)
        return;
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 16;
        FsSubvolInfo *nl = realloc(l->list, nc * sizeof *nl);
        if (!nl)
            return;
        l->list = nl;
        l->cap = nc;
    }
    l->list[l->n++] = *info;
}

static int newest_first(const void *a, const void *b)
{
    const FsSubvolInfo *x = a, *y = b;
    if (x->otransid != y->otransid)
        return x->otransid > y->otransid ? -1 : 1;
    return x->id > y->id ? -1 : x->id < y->id;
}

static int list_checkpoints(Btrfs *b, List *l)
{
    FsStat st;
    Fs *t = top(b);
    memset(l, 0, sizeof *l);
    if (!t)
        return -EIO;
    int r = fs_stat(t, CKPT_DIR, &st);
    fs_close(t);
    if (r < 0)
        return r == -ENOENT ? 0 : r;
    l->dir_ino = st.ino;
    r = fs_subvol_list(b, collect, l);
    qsort(l->list, l->n, sizeof *l->list, newest_first);
    return r;
}

int ckpt_name_back(Btrfs *b, uint32_t x, char *name)
{
    List l;
    int r = list_checkpoints(b, &l);
    if (r == 0 && (x == 0 || x > l.n))
        r = -ENOENT;
    if (r == 0)
        strcpy(name, l.list[x - 1].name);
    free(l.list);
    return r;
}

uint32_t ckpt_count(Btrfs *b)
{
    List l;
    uint32_t n = list_checkpoints(b, &l) == 0 ? (uint32_t)l.n : 0;
    free(l.list);
    return n;
}

/* ---------------------------------------------------- rollback, delete */

int ckpt_rollback(Btrfs *b, const char *name)
{
    char src[FS_NAME_MAX + 32], old[FS_NAME_MAX + 2], fresh[32];
    FsStat st;
    uint64_t id, old_id = fs_get_default_subvol(b);
    if (!valid_name(name))
        return -EINVAL;
    int r = live_path(b, old, sizeof old);
    if (r < 0)
        return r;
    Fs *t = top(b);
    if (!t)
        return -EIO;
    ckpt_path(src, sizeof src, name);
    if ((r = fs_subvol_lookup(t, src, &id)) < 0)
        goto out;
    for (unsigned n = 1;; n++) { /* a free name for the new live subvolume */
        snprintf(fresh, sizeof fresh, "/@%u", n);
        if (fs_stat(t, fresh, &st) == -ENOENT)
            break;
    }
    /* 1. A writable copy of the checkpoint (this commits). */
    if ((r = fs_subvol_snapshot(t, src, fresh, 0, &id)) < 0)
        goto out;
    /* 2. It becomes the default subvolume; 3. the old live one goes. */
    if ((r = fs_set_default_subvol(b, id)) < 0)
        goto out;
    if (old_id != BTRFS_FS_TREE_OBJECTID && (r = fs_subvol_delete(t, old)) < 0)
        goto out;
    r = btrfs_commit(b);
out:
    fs_close(t);
    return r;
}

int ckpt_delete(Btrfs *b, const char *name)
{
    char p[FS_NAME_MAX + 32];
    if (!valid_name(name))
        return -EINVAL;
    Fs *t = top(b);
    if (!t)
        return -EIO;
    ckpt_path(p, sizeof p, name);
    int r = fs_subvol_delete(t, p);
    fs_close(t);
    return r;
}

Fs *ckpt_open_fs(Btrfs *b, const char *name)
{
    char p[FS_NAME_MAX + 32];
    uint64_t id;
    if (!valid_name(name))
        return NULL;
    Fs *t = top(b);
    if (!t)
        return NULL;
    ckpt_path(p, sizeof p, name);
    int r = fs_subvol_lookup(t, p, &id);
    fs_close(t);
    return r == 0 ? fs_open_subvol(b, id) : NULL;
}

int ckpt_rollback_back(Btrfs *b, uint32_t x)
{
    char name[CKPT_NAME_MAX + 1];
    int r = ckpt_name_back(b, x, name);
    return r ? r : ckpt_rollback(b, name);
}

/* ----------------------------------------------------- at fixed times */

#define AUTO_PREFIX "auto-"

struct CkptTimer {
    Btrfs *b;
    uint64_t interval, next;
    uint32_t keep;
};

CkptTimer *ckpt_timer_create(Btrfs *b, uint64_t interval, uint32_t keep, uint64_t now)
{
    CkptTimer *t = interval && keep ? malloc(sizeof *t) : NULL;
    if (!t)
        return NULL;
    t->b = b;
    t->interval = interval;
    t->keep = keep;
    t->next = (now / interval + 1) * interval;
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

static void delete_old(Btrfs *b, uint32_t keep)
{
    char name[CKPT_NAME_MAX + 1];
    uint32_t autos = 0, x = 1;
    while (ckpt_name_back(b, x, name) == 0) {
        int is_auto = strncmp(name, AUTO_PREFIX, strlen(AUTO_PREFIX)) == 0;
        if (is_auto && ++autos > keep)
            ckpt_delete(b, name); /* position x now holds the next older one */
        else
            x++;
    }
}

int ckpt_timer_tick(CkptTimer *t, uint64_t now)
{
    char name[CKPT_NAME_MAX + 1];
    if (now < t->next)
        return 0;
    uint64_t due = now / t->interval * t->interval;
    t->next = due + t->interval;
    delete_old(t->b, t->keep - 1);
    snprintf(name, sizeof name, AUTO_PREFIX "%llu", (unsigned long long)due);
    int r = ckpt_create(t->b, name);
    if (r == -EEXIST)
        return 0;
    return r ? r : 1;
}
