#include "cowtree.h"
#include <string.h>

unsigned long cowtree_copies, cowtree_inplace;

static void node_read(Disk *d, uint32_t b, Node *n)
{
    uint8_t buf[BLOCK_SIZE];
    disk_read(d, b, buf);
    memcpy(n, buf, sizeof *n);
}

static void node_write(Disk *d, uint32_t b, const Node *n)
{
    uint8_t buf[BLOCK_SIZE] = {0};
    memcpy(buf, n, sizeof *n);
    disk_write(d, b, buf);
}

static uint32_t page_index(uint32_t page, int level)
{
    return (page >> (2 * (level - 1))) & (FANOUT - 1);
}

uint32_t cowtree_get(const CowTree *t, uint32_t page)
{
    uint32_t b = t->root;
    for (int level = DEPTH; level >= 1 && b; level--) {
        Node n;
        node_read(t->disk, b, &n);
        b = n.child[page_index(page, level)];
    }
    return b;
}

void cowtree_drop(Disk *d, Allocator *a, uint32_t b, int level)
{
    if (b == 0) return;
    if (alloc_unref(a, b) > 0) return;     /* still shared: stop here */
    if (level == 0) return;                /* a data block */
    Node n;
    node_read(d, b, &n);
    for (int i = 0; i < FANOUT; i++)
        cowtree_drop(d, a, n.child[i], level - 1);
}

/* Return a block holding node `b` that is safe to modify, with its
 * contents in *n. This is the "copy or not" rule. */
static uint32_t node_cow(CowTree *t, uint32_t b, int level, Node *n)
{
    if (b == 0) {                                   /* new, empty node */
        memset(n, 0, sizeof *n);
        return alloc_block(t->alloc);
    }
    node_read(t->disk, b, n);
    if (alloc_refs(t->alloc, b) == 1 && !alloc_is_committed(t->alloc, b)) {
        cowtree_inplace++;
        return b;                                   /* only we see it */
    }
    /* Shared, or part of the last commit: copy it. The copy is a new
     * parent of every child; then the old node loses our reference. */
    uint32_t copy = alloc_block(t->alloc);
    for (int i = 0; i < FANOUT; i++)
        if (n->child[i]) alloc_ref(t->alloc, n->child[i]);
    cowtree_drop(t->disk, t->alloc, b, level);
    cowtree_copies++;
    return copy;
}

static uint32_t set_rec(CowTree *t, uint32_t b, int level, uint32_t page, uint32_t blk)
{
    Node n;
    uint32_t nb = node_cow(t, b, level, &n);     /* top-down: parent first */
    uint32_t i = page_index(page, level);
    if (level == 1) {
        uint32_t old = n.child[i];
        n.child[i] = blk;
        cowtree_drop(t->disk, t->alloc, old, 0);
    } else {
        n.child[i] = set_rec(t, n.child[i], level - 1, page, blk);
    }
    node_write(t->disk, nb, &n);
    return nb;
}

void cowtree_set(CowTree *t, uint32_t page, uint32_t blk)
{
    t->root = set_rec(t, t->root, DEPTH, page, blk);
}

static void visit_rec(Disk *d, uint32_t b, int level, cowtree_visit_fn fn, void *ctx)
{
    if (b == 0) return;
    if (!fn(b, level, ctx) || level == 0) return;
    Node n;
    node_read(d, b, &n);
    for (int i = 0; i < FANOUT; i++)
        visit_rec(d, n.child[i], level - 1, fn, ctx);
}

void cowtree_visit(Disk *d, uint32_t root, cowtree_visit_fn fn, void *ctx)
{
    visit_rec(d, root, DEPTH, fn, ctx);
}
