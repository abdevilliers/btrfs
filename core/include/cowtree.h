/*
 * cowtree.h - CowTree: abstract data type for a copy-on-write tree.
 *
 * The tree maps a page number to the disk block holding that page's data.
 * It has a fixed shape, like an inode's block map:
 *
 *            root                 one node: TREE_FANOUT leaf pointers
 *         /   |   \
 *     leaf  leaf  leaf            each: TREE_FANOUT data-block pointers
 *     /  \    ...
 *   data data                     page n is in leaf n / FANOUT, slot n % FANOUT
 *
 * A tree is named by its root block, so many trees (the live volume and
 * every checkpoint) can share nodes and data blocks. Sharing is recorded
 * in the Allocator's reference counts:
 *
 *   cowtree_share  - one more tree uses this whole tree (a checkpoint)
 *   cowtree_set    - change a page; nodes shared with another tree are
 *                    COPIED first (copy-on-write), never changed in place
 *   cowtree_drop   - one tree stops using it; frees what nobody else uses
 */
#ifndef COWTREE_H
#define COWTREE_H

#include <stdint.h>

#include "allocator.h"
#include "disk.h"

#define TREE_FANOUT 4                            /* pointers per node */
#define TREE_PAGES (TREE_FANOUT * TREE_FANOUT)   /* pages per tree    */

typedef struct CowTree CowTree;

CowTree *cowtree_new(Disk *disk, Allocator *alloc);
void cowtree_free(CowTree *t);

/* Make an empty tree; returns its root block (0 if the disk is full). */
uint32_t cowtree_create(CowTree *t);

/* Data block of `page`, or 0 if the page is empty. */
uint32_t cowtree_get(CowTree *t, uint32_t root, uint32_t page);

/* Point `page` at `data` (0 = erase the page). The tree takes over the
 * caller's reference to `data` and drops its reference to the old block.
 * Copies shared nodes first, so *root may change. -ENOSPC if full. */
int cowtree_set(CowTree *t, uint32_t *root, uint32_t page, uint32_t data);

void cowtree_share(CowTree *t, uint32_t root);
void cowtree_drop(CowTree *t, uint32_t root);

/* Checking: add the references this tree's nodes hold to expected[];
 * seen[] makes shared blocks count once. */
void cowtree_count_refs(CowTree *t, uint32_t root, uint32_t *expected, uint8_t *seen);

/* Print the tree with every block's reference count. */
void cowtree_print(CowTree *t, uint32_t root);

#endif
