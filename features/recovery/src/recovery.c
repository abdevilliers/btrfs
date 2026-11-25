/*
 * recovery.c - mounting after a crash, falling back the way an
 * administrator would. The crash safety itself is in the core: copy-on-write,
 * pinned extents, and the commit order in transaction.c.
 */
#include <errno.h>
#include <stddef.h>

#include "recovery.h"

int rec_commit(Btrfs *b)
{
    return btrfs_commit(b);
}

static int try_mount(Disk *disk, Btrfs **out, int *how, int base)
{
    BtrfsMountOpts backup = { 1, 0 };
    int r = btrfs_mount(disk, NULL, out);
    if (r == 0) {
        *how = base;
        return 0;
    }
    if (r == -ENOENT)
        return r; /* no valid primary superblock at all */
    r = btrfs_mount(disk, &backup, out); /* -o usebackuproot */
    if (r == 0)
        *how = base ? base : 1;
    return r;
}

int rec_mount(Disk *disk, Btrfs **out, int *how)
{
    int dummy;
    if (!how)
        how = &dummy;
    int r = try_mount(disk, out, how, 0);
    if (r == 0)
        return 0;
    /* btrfs rescue super-recover (even if the primary looks like no
     * filesystem at all: a mirror may still be good), then once more. */
    int rr = btrfs_super_recover(disk);
    if (rr <= 0)
        return r == -ENOENT ? -ENOENT : -EIO;
    r = try_mount(disk, out, how, 2);
    return r < 0 ? -EIO : 0;
}

uint64_t rec_generation(Disk *disk)
{
    return btrfs_disk_generation(disk);
}
