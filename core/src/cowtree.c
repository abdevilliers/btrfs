/*
 * cowtree.c - the copy-on-write tree. All of checkpointing rests on
 * three small functions here: node_cow, cowtree_set and drop.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cowtree.h"

/* A node is one disk block holding TREE_FANOUT child block numbers. */
typedef struct {
    uint32_t child[TREE_FANOUT]; /* 0 = empty */
} Node;

_Static_assert(sizeof(Node) <= BLOCK_SIZE, "a node must fit in one block");

/* Levels: the root's children are leaves, the leaves' children are data. */
enum { LEVEL_DATA = -1, LEVEL_LEAF = 0, LEVEL_ROOT = 1 };

struct CowTree {
    Disk *disk;
    Allocator *alloc;
};

CowTree *cowtree_new(Disk *disk, Allocator *alloc)
{
    CowTree *t = malloc(sizeof *t);
    if (t) {
        t->disk = disk;
        t->alloc = alloc;
    }
    return t;
}

void cowtree_free(CowTree *t)
{
    free(t);
}

static void node_read(CowTree *t, uint32_t blk, Node *n)
{
    uint8_t buf[BLOCK_SIZE];
    disk_read(t->disk, blk, buf);
    memcpy(n, buf, sizeof *n);
}

static void node_write(CowTree *t, uint32_t blk, const Node *n)
{
    uint8_t buf[BLOCK_SIZE] = { 0 };
    memcpy(buf, n, sizeof *n);
    disk_write(t->disk, blk, buf);
}

uint32_t cowtree_create(CowTree *t)
{
    Node empty = { { 0 } };
    uint32_t root = alloc_block(t->alloc);
    if (root)
        node_write(t, root, &empty);
    return root;
}

uint32_t cowtree_get(CowTree *t, uint32_t root, uint32_t page)
{
    Node r, leaf;
    if (page >= TREE_PAGES)
        return 0;
    node_read(t, root, &r);
    if (r.child[page / TREE_FANOUT] == 0)
        return 0;
    node_read(t, r.child[page / TREE_FANOUT], &leaf);
    return leaf.child[page % TREE_FANOUT];
}

/*
 * THE copy-on-write rule. Returns the block where the caller may write its
 * changed version of node `n`, which is stored in block `blk`:
 *
 *   1. refs == 1, not in the last commit
 *        Only this tree uses the node and no commit needs it: change it
 *        in place.
 *   2. refs > 1
 *        A checkpoint shares it: never touch it. Make a copy. The copy is
 *        a second parent for every child (children +1), and this tree
 *        stops using the original (original -1).
 *   3. refs == 1, but the last commit uses it
 *        Changing it in place would damage the committed state on disk.
 *        Make a copy; the children simply move to the copy (no change to
 *        their counts) and the original is dropped. The allocator keeps
 *        the original's block untouched until the next commit.
 */
static uint32_t node_cow(CowTree *t, uint32_t blk, const Node *n)
{
    int shared = alloc_refcount(t->alloc, blk) > 1;

    if (!shared && !alloc_committed(t->alloc, blk))
        return blk; /* case 1 */

    uint32_t copy = alloc_block(t->alloc); /* cowtree_set checked there is space */
    if (shared) /* case 2 */
        for (int i = 0; i < TREE_FANOUT; i++)
            if (n->child[i])
                alloc_ref(t->alloc, n->child[i]);
    alloc_unref(t->alloc, blk); /* cases 2 and 3 */
    return copy;
}

int cowtree_set(CowTree *t, uint32_t *root, uint32_t page, uint32_t data)
{
    uint32_t li = page / TREE_FANOUT; /* which leaf          */
    uint32_t di = page % TREE_FANOUT; /* which slot in it    */
    Node r, leaf;

    if (page >= TREE_PAGES)
        return -EINVAL;
    if (data == 0 && cowtree_get(t, *root, page) == 0)
        return 0; /* erasing an empty page: nothing to do */
    if (alloc_free(t->alloc) < 2)
        return -ENOSPC; /* we may need a new root and a new leaf */

    /* 1. The root: copy it if a checkpoint shares it. */
    node_read(t, *root, &r);
    uint32_t new_root = node_cow(t, *root, &r);

    /* 2. The leaf on the path: create it, or copy it if shared. */
    uint32_t new_leaf;
    if (r.child[li] == 0) {
        memset(&leaf, 0, sizeof leaf);
        new_leaf = alloc_block(t->alloc);
    } else {
        node_read(t, r.child[li], &leaf);
        new_leaf = node_cow(t, r.child[li], &leaf);
    }

    /* 3. Point the leaf at the new data, and drop the old data's reference:
     *    it is freed unless a checkpoint still uses it. */
    uint32_t old = leaf.child[di];
    leaf.child[di] = data;
    if (old)
        alloc_unref(t->alloc, old);

    /* 4. Write the changed path, leaf first, then the root. */
    node_write(t, new_leaf, &leaf);
    r.child[li] = new_leaf;
    node_write(t, new_root, &r);
    *root = new_root;
    return 0;
}

void cowtree_share(CowTree *t, uint32_t root)
{
    alloc_ref(t->alloc, root);
}

/* Drop one reference to `blk`. Only when it was the last reference is the
 * block opened and its children dropped too; a block that another tree
 * still uses stops the walk, so shared parts are never visited. */
static void drop(CowTree *t, uint32_t blk, int level)
{
    if (alloc_refcount(t->alloc, blk) > 1) {
        alloc_unref(t->alloc, blk);
        return;
    }
    if (level != LEVEL_DATA) {
        Node n;
        node_read(t, blk, &n);
        for (int i = 0; i < TREE_FANOUT; i++)
            if (n.child[i])
                drop(t, n.child[i], level - 1);
    }
    alloc_unref(t->alloc, blk); /* 1 -> 0: free */
}

void cowtree_drop(CowTree *t, uint32_t root)
{
    drop(t, root, LEVEL_ROOT);
}

static void count(CowTree *t, uint32_t blk, int level, uint32_t *expected, uint8_t *seen)
{
    if (seen[blk])
        return; /* shared: its references were counted already */
    seen[blk] = 1;
    if (level == LEVEL_DATA)
        return;
    Node n;
    node_read(t, blk, &n);
    for (int i = 0; i < TREE_FANOUT; i++)
        if (n.child[i]) {
            expected[n.child[i]]++;
            count(t, n.child[i], level - 1, expected, seen);
        }
}

void cowtree_count_refs(CowTree *t, uint32_t root, uint32_t *expected, uint8_t *seen)
{
    count(t, root, LEVEL_ROOT, expected, seen);
}

static void print_block(CowTree *t, const char *what, uint32_t blk)
{
    uint32_t refs = alloc_refcount(t->alloc, blk);
    printf("%s #%-3u refs %u%s", what, blk, refs, refs > 1 ? "  shared" : "");
}

void cowtree_print(CowTree *t, uint32_t root)
{
    Node r, leaf;
    printf("  ");
    print_block(t, "root", root);
    printf("\n");
    node_read(t, root, &r);
    for (int li = 0; li < TREE_FANOUT; li++) {
        if (!r.child[li])
            continue;
        printf("    ");
        print_block(t, "leaf", r.child[li]);
        printf("   (pages %d-%d)\n", li * TREE_FANOUT, li * TREE_FANOUT + TREE_FANOUT - 1);
        node_read(t, r.child[li], &leaf);
        for (int di = 0; di < TREE_FANOUT; di++) {
            uint32_t d = leaf.child[di];
            char text[BLOCK_SIZE];
            if (!d)
                continue;
            disk_read(t->disk, d, text);
            text[BLOCK_SIZE - 1] = '\0';
            printf("      page %-2d -> ", li * TREE_FANOUT + di);
            print_block(t, "data", d);
            printf("   \"%s\"\n", text);
        }
    }
}
