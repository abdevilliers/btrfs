/* disk.h - an abstract block device (btrfs counterpart: the block device).
 *
 * A Disk is a base struct plus a table of methods. A concrete disk
 * (RAM disk, file disk) embeds `Disk` as its first member and fills in
 * the table. Everything above this layer only calls disk_read/disk_write.
 */
#ifndef DISK_H
#define DISK_H

#include <stdint.h>

#define BLOCK_SIZE 4096u   /* bytes per block */
#define NBLOCKS    1024u   /* blocks per disk -> a 4 MiB disk */

typedef struct Disk Disk;

typedef struct DiskOps {
    void (*read)(Disk *, uint32_t blk, void *buf);
    void (*write)(Disk *, uint32_t blk, const void *buf);
    void (*flush)(Disk *);
    void (*destroy)(Disk *);
} DiskOps;                       /* abstract methods */

struct Disk {
    const DiskOps *ops;
    uint32_t nblocks;
    unsigned long reads, writes; /* statistics, shown by the shell */
};

void disk_read(Disk *d, uint32_t blk, void *buf);
void disk_write(Disk *d, uint32_t blk, const void *buf);
void disk_flush(Disk *d);
void disk_destroy(Disk *d);

/* Concrete disks */
Disk *ramdisk_create(void);                          /* lives in memory */
Disk *filedisk_open(const char *path, int *existed); /* survives restarts */

#endif
