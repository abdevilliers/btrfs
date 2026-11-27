/* recovery.h - survive a crash by never overwriting the last commit
 * (btrfs counterpart: transaction commit + superblock + mount).
 *
 * rec_commit writes, in a safe order:
 *   1. (new tree/data blocks are already on disk, in free space only)
 *   2. root table + refcount table into the SPARE slot
 *   3. flush
 *   4. superblock with generation+1, flush, then the mirror, flush
 *   5. committed = current counts (freed blocks become reusable)
 * A crash at any point leaves either the old or the new commit intact.
 *
 * rec_mount reads both superblocks, keeps those with a valid checksum
 * and takes the one with the highest generation.
 */
#ifndef RECOVERY_H
#define RECOVERY_H

#include "volume.h"

#define SB_MAGIC 0x42545246u   /* "BTRF" */

typedef struct {
    uint32_t magic, csum;
    uint64_t generation;       /* commit number */
    uint32_t live_root;
    uint32_t slot;             /* which slot holds table + refcounts */
    uint32_t nblocks, block_size;
} Superblock;

Volume *rec_format(Disk *d);              /* empty fs, committed as gen 1 */
int     rec_commit(Volume *v);
/* Returns 0 if the primary superblock was used, 1 for the mirror,
 * -1 if neither is valid (not formatted). */
int     rec_mount(Disk *d, Volume **out);

uint32_t crc32(const void *data, uint32_t len);

#endif
