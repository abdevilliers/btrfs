#include "volume.h"
#include <stdlib.h>
#include <string.h>

Volume *vol_create(Disk *d)
{
    Volume *v = calloc(1, sizeof *v);
    v->disk = d;
    v->alloc = alloc_create(d->nblocks, FIRST_FREE);
    v->live = (CowTree){ d, v->alloc, 0 };
    v->next_id = 1;
    return v;
}

void vol_free(Volume *v)
{
    if (!v) return;
    alloc_destroy(v->alloc);
    free(v);
}

CowTree vol_tree(Volume *v, uint32_t root) { return (CowTree){ v->disk, v->alloc, root }; }

void vol_write_page(Volume *v, uint32_t page, const void *data)
{
    uint32_t b = alloc_block(v->alloc);   /* never overwrite: new block */
    disk_write(v->disk, b, data);
    cowtree_set(&v->live, page, b);       /* copies the path, drops old block */
}

void vol_read_page(const CowTree *t, uint32_t page, void *buf)
{
    uint32_t b = cowtree_get(t, page);
    if (b) disk_read(t->disk, b, buf);
    else   memset(buf, 0, BLOCK_SIZE);
}

/* ---- vol_check ---- */
typedef struct { uint32_t *expected; uint8_t *seen; } CheckCtx;

static int count_visit(uint32_t b, int level, void *p)
{
    (void)level;
    CheckCtx *c = p;
    c->expected[b]++;
    if (c->seen[b]) return 0;   /* a shared node: count its children once */
    c->seen[b] = 1;
    return 1;
}

int vol_check(Volume *v)
{
    uint32_t n = alloc_nblocks(v->alloc);
    CheckCtx c = { calloc(n, sizeof(uint32_t)), calloc(n, 1) };
    cowtree_visit(v->disk, v->live.root, count_visit, &c);
    for (int i = 0; i < MAX_CKPT; i++)
        if (v->table[i].used)
            cowtree_visit(v->disk, v->table[i].root, count_visit, &c);
    int bad = 0;
    for (uint32_t b = alloc_first(v->alloc); b < n; b++)
        if (c.expected[b] != alloc_refs(v->alloc, b)) bad++;
    free(c.expected);
    free(c.seen);
    return bad;
}
