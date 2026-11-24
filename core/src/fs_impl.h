/*
 * fs_impl.h - the filesystem's private parts: on-disk structures and the
 * internal algorithms shared by fs_*.c. Not visible to features.
 *
 *   fs_block.c   bread/bwrite, the superblock, alloc and free (data blocks)
 *   fs_inode.c   iget/iupdate, ialloc, ifree, itrunc, bmap
 *   fs_dir.c     directories and path lookup (namei)
 *   fs.c         mkfs and the file operations
 *   fs_check.c   fsck
 */
#ifndef FS_IMPL_H
#define FS_IMPL_H

#include <stdint.h>

#include "fs.h"

/* ---------------------------------------------------- on-disk layout */

#define FS_MAGIC 0x53464f43u /* "COFS" */
#define SUPER_BLOCK 0
#define INODE_START 1

/* Block 0: the superblock (120 bytes). */
typedef struct {
    uint32_t magic;
    uint32_t ninodes;               /* inodes in the table                      */
    uint32_t nblocks;               /* blocks in the filesystem                 */
    uint32_t data_start;            /* first data block                         */
    uint32_t root_ino;              /* inode of "/"                             */
    uint32_t tfree;                 /* total free data blocks                   */
    uint32_t tinode;                /* total free inodes                        */
    uint32_t nfree;                 /* entries used in free[]                   */
    uint32_t free[FS_NICFREE];      /* free block numbers; free[0] = the block
                                       holding the next part of the list, or 0
                                       at the end of the list                   */
    uint32_t ninode;                /* entries used in inode[]                  */
    uint32_t inode[FS_NICINOD];     /* free inode numbers                       */
    uint32_t rinode;                /* remembered inode: ialloc searches from it */
} Super;

/* An inode in the inode table (64 bytes). */
typedef struct {
    uint16_t type;              /* FS_FREE, FS_FILE or FS_DIR                */
    uint16_t nlink;             /* directory entries pointing here           */
    uint32_t size;              /* bytes                                     */
    uint32_t pad[2];
    uint32_t direct[FS_DIRECT]; /* data block of each file block (0 = none)  */
} Dinode;

/* A free-list block: a saved copy of the superblock's free block list. */
typedef struct {
    uint32_t nfree;
    uint32_t free[FS_NICFREE];
} FreeBlock;

/* A directory entry (32 bytes). */
typedef struct {
    uint32_t ino;               /* 0 = empty slot */
    char name[FS_NAME_MAX + 1];
} Dirent;

#define IPB ((uint32_t)(BLOCK_SIZE / sizeof(Dinode)))  /* inodes per block: 2  */
#define DPB ((uint32_t)(BLOCK_SIZE / sizeof(Dirent)))  /* entries per block: 4 */
#define INODE_BLOCKS (FS_INODES / IPB)                 /* 32                   */
#define DATA_START (INODE_START + INODE_BLOCKS)        /* 33                   */
#define NBLOCKS ((uint32_t)VOL_PAGES)                  /* 1024                 */

/* --------------------------------------------------- in-core state */

struct Fs {
    Volume *v;
    uint32_t root;  /* 0 = the live tree; else a fixed tree, read-only     */
    Super sb;       /* in-core copy of the superblock, as in Unix: loaded
                       at the start of each operation, written back at the
                       end if it changed                                   */
    int sb_dirty;   /* the "superblock modified" flag                      */
};

#define TRY(x)               \
    do {                     \
        int r_ = (x);        \
        if (r_ < 0)          \
            return r_;       \
    } while (0)

/* fs_block.c */
void bread(Fs *fs, uint32_t bno, void *buf);           /* empty block = zeros */
int bwrite(Fs *fs, uint32_t bno, const void *buf);
int bclear(Fs *fs, uint32_t bno);                      /* zero: drop its disk block */
void super_load(Fs *fs);
int super_store(Fs *fs);
int balloc(Fs *fs, uint32_t *bno);                     /* Bach: alloc */
int bfree(Fs *fs, uint32_t bno);                       /* Bach: free  */

/* fs_inode.c */
void iget(Fs *fs, uint32_t ino, Dinode *ip);
int iupdate(Fs *fs, uint32_t ino, const Dinode *ip);
int ialloc(Fs *fs, FsType type, uint32_t *ino);
void ifree(Fs *fs, uint32_t ino);
int itrunc(Fs *fs, Dinode *ip, uint32_t from);         /* free blocks >= from */
int iremove(Fs *fs, uint32_t ino, Dinode *ip);         /* itrunc + type 0 + ifree */
int bmap(Fs *fs, Dinode *ip, uint32_t index, int alloc, uint32_t *bno);

/* fs_dir.c */
int dirlookup(Fs *fs, const Dinode *dp, const char *name, uint32_t *ino);
int dirlink(Fs *fs, uint32_t dino, const char *name, uint32_t ino);
int dirunlink(Fs *fs, uint32_t dino, const char *name);
int dirempty(Fs *fs, const Dinode *dp);
int namei(Fs *fs, const char *path, uint32_t *ino);
int nameiparent(Fs *fs, const char *path, uint32_t *parent, char name[FS_NAME_MAX + 1]);

#endif
