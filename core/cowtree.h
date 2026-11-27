/* cowtree.h - a copy-on-write tree mapping page number -> data block
 * (btrfs counterpart: a subvolume's FS tree).
 *
 * Shape: DEPTH levels of nodes, each node has FANOUT children.
 * Level-1 nodes point at data blocks; higher nodes point at nodes.
 * A child of 0 means "empty". With FANOUT 4 and DEPTH 4 a tree holds
 * 4^4 = 256 pages.
 *
 * Copy-on-write rule (node_cow, see slide "copy or not"):
 *   refs == 1 and not committed -> change in place
 *   otherwise                   -> copy the node, children +1,
 *                                  old node -1 (freed if it hits 0)
 */
#ifndef COWTREE_H
#define COWTREE_H

#include <stdint.h>
#include "disk.h"
#include "allocator.h"

#define FANOUT    4
#define DEPTH     4
#define MAX_PAGES 256u   /* FANOUT^DEPTH */

typedef struct { uint32_t child[FANOUT]; } Node;   /* one node per block */

typedef struct {
    Disk      *disk;
    Allocator *alloc;
    uint32_t   root;    /* 0 = empty tree */
} CowTree;

/* Data block of `page`, or 0 if the page was never written. */
uint32_t cowtree_get(const CowTree *t, uint32_t page);

/* Point `page` at `blk`. The caller already holds one reference on blk;
 * that reference moves into the tree. The page's old block loses one.
 * blk == 0 clears the page. */
void cowtree_set(CowTree *t, uint32_t page, uint32_t blk);

/* Drop one reference to a subtree; frees whatever only it used. */
void cowtree_drop(Disk *d, Allocator *a, uint32_t blk, int level);

/* Visit every block reachable from root. If visit() returns 0 the
 * children of that node are not visited. */
typedef int (*cowtree_visit_fn)(uint32_t blk, int level, void *ctx);
void cowtree_visit(Disk *d, uint32_t root, cowtree_visit_fn visit, void *ctx);

/* Statistics: how many nodes were copied vs changed in place. */
extern unsigned long cowtree_copies, cowtree_inplace;

#endif
