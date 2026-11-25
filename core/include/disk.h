/*
 * disk.h - Disk: abstract data type for a simulated disk.
 *
 * A disk is an array of fixed-size blocks. These functions call the
 * "abstract methods" of a concrete disk (see mem_disk.h), so code above
 * this layer never knows how the blocks are really stored.
 *
 * Block number 0 is never used for data: 0 means "no block".
 */
#ifndef DISK_H
#define DISK_H

#include <stdint.h>

#define BLOCK_SIZE 4096u /* one sector: the unit the disk reads and writes */

typedef struct Disk Disk;

int disk_read(Disk *d, uint32_t blk, void *buf);        /* BLOCK_SIZE bytes */
int disk_write(Disk *d, uint32_t blk, const void *buf);
int disk_flush(Disk *d);                               /* push writes to the medium */
uint32_t disk_blocks(const Disk *d);
uint64_t disk_write_count(const Disk *d);               /* blocks written so far */
void disk_close(Disk *d);

#endif
