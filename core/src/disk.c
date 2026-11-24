/*
 * disk.c - the abstract Disk: checks arguments, counts writes, and
 * dispatches to the concrete disk's methods.
 */
#include <errno.h>

#include "disk_impl.h"

void disk_init(Disk *d, const DiskOps *ops, uint32_t nblocks)
{
    d->ops = ops;
    d->nblocks = nblocks;
    d->writes = 0;
}

int disk_read(Disk *d, uint32_t blk, void *buf)
{
    if (blk == 0 || blk >= d->nblocks)
        return -EINVAL;
    d->ops->read(d, blk, buf);
    return 0;
}

int disk_write(Disk *d, uint32_t blk, const void *buf)
{
    if (blk == 0 || blk >= d->nblocks)
        return -EINVAL;
    d->writes++;
    d->ops->write(d, blk, buf);
    return 0;
}

int disk_flush(Disk *d)
{
    d->ops->flush(d);
    return 0;
}

uint32_t disk_blocks(const Disk *d)
{
    return d->nblocks;
}

uint64_t disk_write_count(const Disk *d)
{
    return d->writes;
}

void disk_close(Disk *d)
{
    if (d)
        d->ops->destroy(d);
}
