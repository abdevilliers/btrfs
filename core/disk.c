#define _POSIX_C_SOURCE 200809L
#include "disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check_blk(Disk *d, uint32_t blk)
{
    if (blk >= d->nblocks) {
        fprintf(stderr, "disk: block %u out of range\n", blk);
        abort();
    }
}

void disk_read(Disk *d, uint32_t b, void *buf)        { check_blk(d, b); d->reads++;  d->ops->read(d, b, buf); }
void disk_write(Disk *d, uint32_t b, const void *buf) { check_blk(d, b); d->writes++; d->ops->write(d, b, buf); }
void disk_flush(Disk *d)   { d->ops->flush(d); }
void disk_destroy(Disk *d) { if (d) d->ops->destroy(d); }

/* ---------------- RAM disk ---------------- */
typedef struct { Disk base; uint8_t *mem; } RamDisk;

static void ram_read(Disk *d, uint32_t b, void *buf)        { memcpy(buf, ((RamDisk *)d)->mem + (size_t)b * BLOCK_SIZE, BLOCK_SIZE); }
static void ram_write(Disk *d, uint32_t b, const void *buf) { memcpy(((RamDisk *)d)->mem + (size_t)b * BLOCK_SIZE, buf, BLOCK_SIZE); }
static void ram_flush(Disk *d) { (void)d; }
static void ram_destroy(Disk *d) { free(((RamDisk *)d)->mem); free(d); }

static const DiskOps ram_ops = { ram_read, ram_write, ram_flush, ram_destroy };

Disk *ramdisk_create(void)
{
    RamDisk *r = calloc(1, sizeof *r);
    r->mem = calloc(NBLOCKS, BLOCK_SIZE);
    r->base.ops = &ram_ops;
    r->base.nblocks = NBLOCKS;
    return &r->base;
}

/* ---------------- File-backed disk ---------------- */
typedef struct { Disk base; FILE *f; } FileDisk;

static void file_read(Disk *d, uint32_t b, void *buf)
{
    FILE *f = ((FileDisk *)d)->f;
    fseek(f, (long)b * (long)BLOCK_SIZE, SEEK_SET);
    size_t n = fread(buf, 1, BLOCK_SIZE, f);
    if (n < BLOCK_SIZE) memset((uint8_t *)buf + n, 0, BLOCK_SIZE - n);
}
static void file_write(Disk *d, uint32_t b, const void *buf)
{
    FILE *f = ((FileDisk *)d)->f;
    fseek(f, (long)b * (long)BLOCK_SIZE, SEEK_SET);
    fwrite(buf, 1, BLOCK_SIZE, f);
}
static void file_flush(Disk *d)
{
    FILE *f = ((FileDisk *)d)->f;
    fflush(f);
    fsync(fileno(f));
}
static void file_destroy(Disk *d) { fclose(((FileDisk *)d)->f); free(d); }

static const DiskOps file_ops = { file_read, file_write, file_flush, file_destroy };

Disk *filedisk_open(const char *path, int *existed)
{
    FILE *f = fopen(path, "r+b");
    if (existed) *existed = (f != NULL);
    if (!f) {
        f = fopen(path, "w+b");
        if (!f) return NULL;
        if (ftruncate(fileno(f), (off_t)NBLOCKS * BLOCK_SIZE) != 0) { fclose(f); return NULL; }
    }
    FileDisk *fd = calloc(1, sizeof *fd);
    fd->f = f;
    fd->base.ops = &file_ops;
    fd->base.nblocks = NBLOCKS;
    return &fd->base;
}
