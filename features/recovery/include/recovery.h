/*
 * recovery.h - make the filesystem survive a crash, and get it back.
 *
 * btrfs never overwrites anything the last commit uses: new tree blocks
 * and data go to free space, and space freed in a transaction stays
 * reserved ("pinned") until the transaction has committed. A commit
 * writes every new block, flushes, then writes the superblock (all its
 * copies) pointing at the new root tree. A crash at any moment leaves
 * either the old commit or the new one.
 *
 * rec_mount brings back the last commit, trying what an administrator
 * would, in order:
 *   1. mount;
 *   2. mount -o usebackuproot: the newest root tree that still reads
 *      cleanly, from the four backup roots in the superblock;
 *   3. btrfs rescue super-recover (if the primary superblock is damaged,
 *      copy the newest good mirror over it), then 1 and 2 again.
 *
 * Uses only the public core API (fs.h).
 */
#ifndef RECOVERY_H
#define RECOVERY_H

#include <stdint.h>

#include "fs.h"

int rec_commit(Btrfs *b);

/* -ENOENT if the disk holds no filesystem; -EIO if nothing could be
 * recovered. *how (may be NULL) = 0 plain mount, 1 backup root, 2 after
 * super-recover. */
int rec_mount(Disk *disk, Btrfs **out, int *how);

uint64_t rec_generation(Disk *disk); /* the last commit's transid; 0 if none */

#endif
