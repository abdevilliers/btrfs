/*
 * fault_disk.c - a Disk that forwards to another and can drop writes.
 */
#include <stdlib.h>

#include "disk_impl.h"
#include "fault_disk.h"

typedef struct {
    Disk base; /* must be first */
    Disk *inner;
    int cut;            /* counting down to a power cut */
    uint64_t left;      /* writes still allowed         */
    uint64_t dropped;
} FaultDisk;

static void fd_read(Disk *self, uint32_t blk, void *buf)
{
    disk_read(((FaultDisk *)self)->inner, blk, buf);
}

static void fd_write(Disk *self, uint32_t blk, const void *buf)
{
    FaultDisk *f = (FaultDisk *)self;
    if (f->cut) {
        if (f->left == 0) {
            f->dropped++;
            return;
        }
        f->left--;
    }
    disk_write(f->inner, blk, buf);
}

static void fd_flush(Disk *self)
{
    disk_flush(((FaultDisk *)self)->inner);
}

static void fd_destroy(Disk *self)
{
    free(self);
}

static const DiskOps fault_ops = { fd_read, fd_write, fd_flush, fd_destroy };

Disk *fault_disk_wrap(Disk *inner)
{
    FaultDisk *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->inner = inner;
    disk_init(&f->base, &fault_ops, disk_blocks(inner));
    return &f->base;
}

void fault_disk_cut_after(Disk *d, uint64_t writes)
{
    FaultDisk *f = (FaultDisk *)d;
    f->cut = 1;
    f->left = writes;
}

void fault_disk_restore(Disk *d)
{
    ((FaultDisk *)d)->cut = 0;
}

uint64_t fault_disk_dropped(const Disk *d)
{
    return ((const FaultDisk *)d)->dropped;
}
