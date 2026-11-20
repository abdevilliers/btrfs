/*
 * mem_disk.c - Disk implementation backed by a RAM buffer.
 */
#include <stdlib.h>
#include <string.h>

#include "disk_impl.h"
#include "mem_disk.h"

typedef struct {
    Disk base; /* must be first, so a MemDisk* can be used as a Disk* */
    uint8_t *data;
} MemDisk;

static void mem_read(Disk *self, uint32_t blk, void *buf)
{
    memcpy(buf, ((MemDisk *)self)->data + (size_t)blk * BLOCK_SIZE, BLOCK_SIZE);
}

static void mem_write(Disk *self, uint32_t blk, const void *buf)
{
    memcpy(((MemDisk *)self)->data + (size_t)blk * BLOCK_SIZE, buf, BLOCK_SIZE);
}

static void mem_flush(Disk *self)
{
    (void)self; /* RAM: nothing to push */
}

static void mem_destroy(Disk *self)
{
    free(((MemDisk *)self)->data);
    free(self);
}

static const DiskOps mem_ops = { mem_read, mem_write, mem_flush, mem_destroy };

Disk *mem_disk_create(uint32_t nblocks)
{
    MemDisk *m = malloc(sizeof *m);
    if (!m)
        return NULL;
    m->data = calloc(nblocks, BLOCK_SIZE);
    if (!m->data) {
        free(m);
        return NULL;
    }
    disk_init(&m->base, &mem_ops, nblocks);
    return &m->base;
}
