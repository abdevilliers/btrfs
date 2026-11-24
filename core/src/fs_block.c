/*
 * fs_block.c - block I/O, the superblock, and the free block list:
 * Bach's algorithms "alloc" (balloc) and "free" (bfree).
 *
 * The free data blocks form a chained list:
 *
 *   superblock                 block 40 (a free block)       block 52
 *   nfree = 4                  nfree = 12                     nfree = 12
 *   free[0] = 40  ----------->  free[0] = 52  -------------->  free[0] = 0  (end)
 *   free[1] = 35                free[1..11] = 11 more free     free[1..11] = ...
 *   free[2] = 36                blocks
 *   free[3] = 37  <- next to be allocated
 *
 * free[0] of each list is the block that holds the next list; every other
 * entry is a free block. All of these blocks are free, including the ones
 * that hold a part of the list.
 */
#include <errno.h>
#include <string.h>

#include "fs_impl.h"

/* ------------------------------------------------------------- block I/O */

void bread(Fs *fs, uint32_t bno, void *buf)
{
    if (fs->root)
        vol_read_page_tree(fs->v, fs->root, bno, buf);
    else
        vol_read_page(fs->v, bno, buf);
}

int bwrite(Fs *fs, uint32_t bno, const void *buf)
{
    /* The volume writes it to a new disk block: copy-on-write. */
    return vol_write_page(fs->v, bno, buf);
}

int bclear(Fs *fs, uint32_t bno)
{
    /* An empty page reads as zeros and holds no disk block. */
    return vol_erase(fs->v, bno);
}

/* ------------------------------------------------------------ superblock */

void super_load(Fs *fs)
{
    uint8_t buf[BLOCK_SIZE];
    bread(fs, SUPER_BLOCK, buf);
    memcpy(&fs->sb, buf, sizeof fs->sb);
    fs->sb_dirty = 0;
}

int super_store(Fs *fs)
{
    uint8_t buf[BLOCK_SIZE] = { 0 };
    memcpy(buf, &fs->sb, sizeof fs->sb);
    TRY(bwrite(fs, SUPER_BLOCK, buf));
    fs->sb_dirty = 0;
    return 0;
}

/* --------------------------------------------------------------- alloc */

/*
 * Bach, algorithm alloc:
 *   remove block from super block free list;
 *   if (removed last block from free list) {
 *       read block just taken from free list;
 *       copy block numbers in block into super block;
 *   }
 *   zero buffer contents;
 *   decrement total count of free blocks;
 *   mark super block modified;
 */
int balloc(Fs *fs, uint32_t *out)
{
    Super *sb = &fs->sb;

    if (sb->nfree == 0)
        return -ENOSPC;
    uint32_t bno = sb->free[sb->nfree - 1];
    if (bno == 0)
        return -ENOSPC; /* reached the end-of-list marker: the disk is full */
    sb->nfree--;

    if (sb->nfree == 0) {
        /* We took free[0], the block holding the next part of the list:
         * copy that list into the superblock, then use the block itself. */
        FreeBlock fb;
        uint8_t buf[BLOCK_SIZE];
        bread(fs, bno, buf);
        memcpy(&fb, buf, sizeof fb);
        if (fb.nfree == 0 || fb.nfree > FS_NICFREE)
            return -EIO; /* a damaged list */
        sb->nfree = fb.nfree;
        memcpy(sb->free, fb.free, sizeof sb->free);
    }

    TRY(bclear(fs, bno)); /* zero the block */
    sb->tfree--;
    fs->sb_dirty = 1;
    *out = bno;
    return 0;
}

/* ---------------------------------------------------------------- free */

/*
 * Bach, algorithm free (Unix V7 "free"):
 *   if (super block list is full) {
 *       write the super block list into the block being freed;
 *       empty the super block list;
 *   }
 *   put the block number in the super block list
 *   (free[0] of a new list = the block holding the old one);
 *   increment total count of free blocks;
 *   mark super block modified;
 */
int bfree(Fs *fs, uint32_t bno)
{
    Super *sb = &fs->sb;

    if (bno < sb->data_start || bno >= sb->nblocks)
        return -EINVAL;
    if (sb->nfree == 0) {
        sb->free[0] = 0; /* a brand-new list: 0 marks its end */
        sb->nfree = 1;
    }
    if (sb->nfree == FS_NICFREE) {
        /* The superblock's list is full: save it in the block being freed.
         * That block becomes free[0] of a new, empty list. */
        FreeBlock fb;
        uint8_t buf[BLOCK_SIZE] = { 0 };
        fb.nfree = sb->nfree;
        memcpy(fb.free, sb->free, sizeof fb.free);
        memcpy(buf, &fb, sizeof fb);
        TRY(bwrite(fs, bno, buf));
        sb->nfree = 0;
    } else {
        TRY(bclear(fs, bno)); /* its old contents are not needed */
    }
    sb->free[sb->nfree++] = bno;
    sb->tfree++;
    fs->sb_dirty = 1;
    return 0;
}
