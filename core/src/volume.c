/*
 * volume.c - the live volume, the root table, consistency check, printing.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "volume.h"

_Static_assert(VOL_PAGES == TREE_PAGES, "a volume is exactly one tree's pages");

typedef struct {
    uint32_t used;
    uint32_t root;
    uint32_t seq;                      /* order of creation: bigger = newer */
    char name[VOL_NAME_MAX + 1];
} RootEntry;

/* Everything the volume keeps in memory. vol_save / vol_load copy this
 * struct as it is; all the rest of the volume is blocks on the disk. */
typedef struct {
    uint32_t root;                     /* root of the live tree          */
    uint32_t next_seq;                 /* seq for the next root entry    */
    RootEntry roots[VOL_MAX_ROOTS];    /* the root table                 */
} State;

_Static_assert(sizeof(State) <= VOL_STATE_MAX, "VOL_STATE_MAX is too small");

struct Volume {
    Disk *disk;
    Allocator *alloc;
    CowTree *tree;
    State s;
};

/* A volume with its building blocks but no tree yet. */
static Volume *vol_new(Disk *disk)
{
    Volume *v = disk_blocks(disk) > VOL_META_BLOCKS + 1 ? calloc(1, sizeof *v) : NULL;
    if (!v)
        return NULL;
    v->disk = disk;
    v->alloc = alloc_create(disk_blocks(disk));
    v->tree = v->alloc ? cowtree_new(disk, v->alloc) : NULL;
    if (!v->tree) {
        vol_destroy(v);
        return NULL;
    }
    for (uint32_t b = 1; b <= VOL_META_BLOCKS; b++)
        alloc_set(v->alloc, b, 1); /* set aside: never handed out */
    return v;
}

/* ------------------------------------------------------------ pages */

Volume *vol_create(Disk *disk)
{
    Volume *v = vol_new(disk);
    if (!v)
        return NULL;
    v->s.root = cowtree_create(v->tree);
    if (!v->s.root) {
        vol_destroy(v);
        return NULL;
    }
    return v;
}

void vol_destroy(Volume *v)
{
    if (v) {
        cowtree_free(v->tree);
        alloc_destroy(v->alloc);
        free(v);
    }
}

int vol_write_page(Volume *v, uint32_t page, const void *buf)
{
    if (page >= VOL_PAGES)
        return -EINVAL;
    if (alloc_free(v->alloc) < 3)
        return -ENOSPC; /* the data block + a copied root + a copied leaf */

    uint32_t data = alloc_block(v->alloc);
    disk_write(v->disk, data, buf);
    return vol_map_page(v, page, data); /* the tree owns it now */
}

int vol_write(Volume *v, uint32_t page, const char *text)
{
    char buf[BLOCK_SIZE] = { 0 };
    snprintf(buf, sizeof buf, "%s", text);
    return vol_write_page(v, page, buf);
}

void vol_read_page_tree(Volume *v, uint32_t root, uint32_t page, void *buf)
{
    uint32_t data = cowtree_get(v->tree, root, page);
    if (data)
        disk_read(v->disk, data, buf);
    else
        memset(buf, 0, BLOCK_SIZE); /* an empty page reads as zeros */
}

void vol_read_page(Volume *v, uint32_t page, void *buf)
{
    vol_read_page_tree(v, v->s.root, page, buf);
}

int vol_erase(Volume *v, uint32_t page)
{
    return vol_map_page(v, page, 0);
}

int vol_map_page(Volume *v, uint32_t page, uint32_t data)
{
    return cowtree_set(v->tree, &v->s.root, page, data);
}

int vol_read_tree(Volume *v, uint32_t root, uint32_t page, char *buf, size_t len)
{
    char block[BLOCK_SIZE];
    uint32_t data = cowtree_get(v->tree, root, page);
    if (!data)
        return -ENOENT;
    disk_read(v->disk, data, block);
    block[BLOCK_SIZE - 1] = '\0';
    snprintf(buf, len, "%s", block);
    return 0;
}

int vol_read(Volume *v, uint32_t page, char *buf, size_t len)
{
    return vol_read_tree(v, v->s.root, page, buf, len);
}

uint32_t vol_used_blocks(const Volume *v)
{
    return alloc_used(v->alloc) - VOL_META_BLOCKS; /* pages and tree nodes only */
}

/* ------------------------------------------------------- live root */

uint32_t vol_live_root(const Volume *v)
{
    return v->s.root;
}

void vol_set_live_root(Volume *v, uint32_t root)
{
    cowtree_share(v->tree, root); /* take the new tree first: it may be the same one */
    cowtree_drop(v->tree, v->s.root);
    v->s.root = root;
}

/* ------------------------------------------------------ root table */

static RootEntry *find(Volume *v, const char *name)
{
    for (int i = 0; i < VOL_MAX_ROOTS; i++)
        if (v->s.roots[i].used && strcmp(v->s.roots[i].name, name) == 0)
            return &v->s.roots[i];
    return NULL;
}

int vol_root_add(Volume *v, const char *name, uint32_t root)
{
    if (!name || !*name || strlen(name) > VOL_NAME_MAX)
        return -EINVAL;
    if (find(v, name))
        return -EEXIST;
    for (int i = 0; i < VOL_MAX_ROOTS; i++) {
        RootEntry *e = &v->s.roots[i];
        if (e->used)
            continue;
        e->used = 1;
        strcpy(e->name, name);
        e->root = root;
        e->seq = ++v->s.next_seq;
        cowtree_share(v->tree, root); /* this entry now holds a reference */
        return 0;
    }
    return -ENOSPC;
}

int vol_root_find(Volume *v, const char *name, uint32_t *root)
{
    RootEntry *e = name ? find(v, name) : NULL;
    if (!e)
        return -ENOENT;
    *root = e->root;
    return 0;
}

int vol_root_remove(Volume *v, const char *name)
{
    RootEntry *e = name ? find(v, name) : NULL;
    if (!e)
        return -ENOENT;
    cowtree_drop(v->tree, e->root); /* frees only what nobody else uses */
    e->used = 0;
    return 0;
}

int vol_root_nth(Volume *v, uint32_t n, char *name, uint32_t *root)
{
    uint32_t below = UINT32_MAX; /* look for the newest entry older than this */
    RootEntry *e = NULL;

    for (uint32_t k = 0; k < n; k++) {
        e = NULL;
        for (int i = 0; i < VOL_MAX_ROOTS; i++) {
            RootEntry *c = &v->s.roots[i];
            if (c->used && c->seq < below && (!e || c->seq > e->seq))
                e = c;
        }
        if (!e)
            return -ENOENT;
        below = e->seq;
    }
    if (!e)
        return -ENOENT; /* n == 0 */
    strcpy(name, e->name);
    *root = e->root;
    return 0;
}

void vol_root_each(Volume *v, vol_root_fn fn, void *arg)
{
    for (int i = 0; i < VOL_MAX_ROOTS; i++)
        if (v->s.roots[i].used)
            fn(arg, v->s.roots[i].name, v->s.roots[i].root);
}

CowTree *vol_tree(Volume *v)
{
    return v->tree;
}

Allocator *vol_allocator(Volume *v)
{
    return v->alloc;
}

Disk *vol_disk(Volume *v)
{
    return v->disk;
}

/* ------------------------------------------------- check and print */

/* Count every reference from scratch by walking all the trees. Returns
 * an array with the count each block should have (the caller frees it),
 * or NULL if out of memory. */
static uint32_t *count_refs(Volume *v)
{
    uint32_t n = disk_blocks(v->disk);
    uint32_t *expected = calloc(n, sizeof *expected);
    uint8_t *seen = calloc(n, 1);

    if (expected && seen) {
        for (uint32_t b = 0; b <= VOL_META_BLOCKS; b++)
            expected[b] = 1;     /* block 0 and the metadata blocks */
        expected[v->s.root]++;   /* the live volume holds its root  */
        cowtree_count_refs(v->tree, v->s.root, expected, seen);
        for (int i = 0; i < VOL_MAX_ROOTS; i++)
            if (v->s.roots[i].used) {
                expected[v->s.roots[i].root]++; /* each entry holds its root */
                cowtree_count_refs(v->tree, v->s.roots[i].root, expected, seen);
            }
    } else {
        free(expected);
        expected = NULL;
    }
    free(seen);
    return expected;
}

int vol_check(Volume *v)
{
    uint32_t *expected = count_refs(v);
    int errors = 0;

    if (!expected)
        return -ENOMEM;
    for (uint32_t b = 0; b < disk_blocks(v->disk); b++)
        if (expected[b] != alloc_refcount(v->alloc, b))
            errors++;
    free(expected);
    return errors;
}

/* ------------------------------------------------- save and load */

int vol_save(const Volume *v, void *buf, size_t len)
{
    if (len < sizeof v->s)
        return -ENOSPC;
    memcpy(buf, &v->s, sizeof v->s);
    return (int)sizeof v->s;
}

static int valid_root(const Volume *v, uint32_t root)
{
    return root > VOL_META_BLOCKS && root < disk_blocks(v->disk);
}

Volume *vol_load(Disk *disk, const void *buf, size_t len)
{
    Volume *v = len >= sizeof v->s ? vol_new(disk) : NULL;
    uint32_t *refs;

    if (!v)
        return NULL;
    memcpy(&v->s, buf, sizeof v->s);

    int ok = valid_root(v, v->s.root);
    for (int i = 0; i < VOL_MAX_ROOTS; i++) {
        RootEntry *e = &v->s.roots[i];
        e->name[VOL_NAME_MAX] = '\0';
        if (e->used && !valid_root(v, e->root))
            ok = 0;
    }

    /* The reference counts are not saved: walking the trees gives them. */
    refs = ok ? count_refs(v) : NULL;
    if (!refs) {
        vol_destroy(v);
        return NULL;
    }
    for (uint32_t b = 1; b < disk_blocks(disk); b++)
        alloc_set(v->alloc, b, refs[b]);
    free(refs);
    return v;
}

void vol_print(Volume *v)
{
    printf("live volume:\n");
    cowtree_print(v->tree, v->s.root);
    for (int i = 0; i < VOL_MAX_ROOTS; i++)
        if (v->s.roots[i].used) {
            printf("\"%s\":\n", v->s.roots[i].name);
            cowtree_print(v->tree, v->s.roots[i].root);
        }
}
