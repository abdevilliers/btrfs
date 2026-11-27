/* fs.h - files and inodes on top of the copy-on-write tree
 * (btrfs counterpart: INODE_ITEM, DIR_ITEM and EXTENT_DATA items in an
 *  FS tree).
 *
 * Every tree (live or checkpoint) is a whole filesystem:
 *   page 0                    inode table: MAX_FILES inodes (one flat
 *                             directory, so the table is also the dir)
 *   page 1 + i*16 + k         page k of the file in inode slot i
 *
 * Because the inode table is itself a page in the tree, a checkpoint of
 * the tree captures file names and sizes together with the data.
 */
#ifndef FS_H
#define FS_H

#include <stdint.h>
#include "volume.h"

#define MAX_FILES       15
#define PAGES_PER_FILE  16
#define MAX_FILE_SIZE   (PAGES_PER_FILE * BLOCK_SIZE)   /* 64 KiB */
#define NAME_LEN        20
#define FIRST_INO       257   /* like btrfs: first user inode is 257 */

typedef struct {
    uint32_t used;
    uint32_t ino;
    uint32_t size;            /* bytes */
    char     name[NAME_LEN];
} Inode;                      /* 32 bytes, 128 fit in a block */

uint32_t fs_data_page(int slot, int k);

/* --- reading: work on any tree, so they also read checkpoints --- */
int  fs_lookup(const CowTree *t, const char *name, Inode *out); /* slot or -1 */
int  fs_list(const CowTree *t, Inode out[MAX_FILES]);           /* count */
long fs_read(const CowTree *t, const char *name, void *buf, uint32_t cap);

/* --- writing: always the live tree --- */
/* Replace the whole file (created if needed). Pages whose contents did
 * not change are not rewritten. Returns pages written, or -1. */
int  fs_write(Volume *v, const char *name, const void *data, uint32_t len);
int  fs_delete(Volume *v, const char *name);

/* Used by duplication: get/create an inode slot, set a file's size. */
int  fs_create(Volume *v, const char *name);    /* slot or -1 if full */
void fs_set_size(Volume *v, int slot, uint32_t size);

#endif
