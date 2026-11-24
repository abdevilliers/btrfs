/*
 * disk_impl.h - PRIVATE: only for code that implements a concrete Disk.
 *
 * A concrete disk embeds `struct Disk` as its first member and fills in
 * the table of abstract methods:
 *
 *     typedef struct { Disk base; uint8_t *data; } MemDisk;
 */
#ifndef DISK_IMPL_H
#define DISK_IMPL_H

#include "disk.h"

typedef struct DiskOps {               /* the abstract methods */
    void (*read)(Disk *self, uint32_t blk, void *buf);
    void (*write)(Disk *self, uint32_t blk, const void *buf);
    void (*flush)(Disk *self);
    void (*destroy)(Disk *self);
} DiskOps;

struct Disk {                          /* the abstract base */
    const DiskOps *ops;
    uint32_t nblocks;
    uint64_t writes;
};

void disk_init(Disk *d, const DiskOps *ops, uint32_t nblocks);

#endif
