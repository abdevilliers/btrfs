/*
 * file_disk.c - Disk implementation backed by a file. Block b lives at
 * byte offset b * BLOCK_SIZE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disk_impl.h"
#include "file_disk.h"

typedef struct {
    Disk base; /* must be first, so a FileDisk* can be used as a Disk* */
    FILE *f;
} FileDisk;

static void file_read(Disk *self, uint32_t blk, void *buf)
{
    FILE *f = ((FileDisk *)self)->f;
    memset(buf, 0, BLOCK_SIZE); /* a block never written reads as zeros */
    if (fseek(f, (long)blk * (long)BLOCK_SIZE, SEEK_SET) == 0) {
        size_t got = fread(buf, 1, BLOCK_SIZE, f);
        (void)got; /* past the end of the file: the zeros stay */
    }
}

static void file_write(Disk *self, uint32_t blk, const void *buf)
{
    FILE *f = ((FileDisk *)self)->f;
    if (fseek(f, (long)blk * (long)BLOCK_SIZE, SEEK_SET) == 0)
        fwrite(buf, 1, BLOCK_SIZE, f);
}

static void file_flush(Disk *self)
{
    /* Hands the data to the operating system. A real filesystem would
     * also call fsync() here to survive a power cut. */
    fflush(((FileDisk *)self)->f);
}

static void file_destroy(Disk *self)
{
    fclose(((FileDisk *)self)->f);
    free(self);
}

static const DiskOps file_ops = { file_read, file_write, file_flush, file_destroy };

Disk *file_disk_open(const char *path, uint32_t nblocks)
{
    FileDisk *d = malloc(sizeof *d);
    if (!d)
        return NULL;
    d->f = fopen(path, "r+b");     /* an existing image ...   */
    if (!d->f)
        d->f = fopen(path, "w+b"); /* ... or start a new one  */
    if (!d->f) {
        free(d);
        return NULL;
    }
    disk_init(&d->base, &file_ops, nblocks);
    return &d->base;
}
