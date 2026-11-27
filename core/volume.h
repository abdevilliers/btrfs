/* volume.h - the live tree + a root table of named trees
 * (btrfs counterpart: the root tree + subvolumes/snapshots).
 *
 * On-disk layout (block = 4 KiB):
 *   0      boot block (unused)
 *   1      superblock            (written by recovery)
 *   2      superblock mirror
 *   3, 4   slot 0: root table, refcount table
 *   5, 6   slot 1: root table, refcount table
 *   7 ..   tree nodes and data blocks, handed out by the allocator
 */
#ifndef VOLUME_H
#define VOLUME_H

#include <stdint.h>
#include "disk.h"
#include "allocator.h"
#include "cowtree.h"

#define SB_PRIMARY   1u
#define SB_MIRROR    2u
#define SLOT_BASE    3u          /* slot s: table at 3+2s, refs at 4+2s */
#define FIRST_FREE   7u

#define MAX_CKPT     16
#define CKPT_NAME    32

/* One entry per checkpoint. It holds one reference to its tree's root. */
typedef struct {
    uint32_t used;
    uint32_t id;           /* creation order: 1, 2, 3 ... */
    uint32_t root;         /* the tree's root block */
    uint32_t pad;
    uint64_t generation;   /* commit number when it was taken */
    char     name[CKPT_NAME];
} RootEntry;

typedef struct {
    Disk      *disk;
    Allocator *alloc;
    CowTree    live;                 /* the writable tree */
    RootEntry  table[MAX_CKPT];      /* the root table: checkpoints */
    uint32_t   next_id;
    uint64_t   generation;           /* last committed generation */
    uint32_t   sb_slot;              /* slot used by the last commit */
} Volume;

Volume *vol_create(Disk *d);      /* empty volume in memory (no disk writes) */
void    vol_free(Volume *v);      /* forget the in-memory state (disk untouched) */

/* Page I/O on the live tree. Every write goes to a NEW data block. */
void vol_write_page(Volume *v, uint32_t page, const void *data);
void vol_read_page(const CowTree *t, uint32_t page, void *buf);

/* A read-only view of any tree root (live or checkpoint). */
CowTree vol_tree(Volume *v, uint32_t root);

/* Consistency check: recompute every refcount from the trees and compare.
 * Returns the number of blocks whose count is wrong (0 = healthy). */
int vol_check(Volume *v);

#endif
