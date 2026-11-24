/*
 * fs.h - Fs: abstract data type for a Unix-style filesystem (files,
 * directories, inodes) stored inside a copy-on-write Volume.
 *
 * The filesystem uses the volume's 1024 pages as its disk blocks
 * (one block = one 128-byte page), laid out like the classic System V /
 * Bach filesystem:
 *
 *   block 0        superblock   sizes, free counts, the free block list
 *                               cache and the free inode list cache
 *   blocks 1..32   inode table  64 inodes of 64 bytes, 2 per block
 *   blocks 33..    data blocks  file contents and directory entries
 *
 * Allocation uses the algorithms from Maurice Bach, "The Design of the
 * UNIX Operating System", chapter 4 (core/src/fs_inode.c, fs_block.c):
 *
 *   ialloc  take a free inode from the superblock's free inode list; when
 *           the list is empty, search the inode table for free inodes,
 *           starting at the "remembered inode"
 *   ifree   put an inode back in the list, or, if the list is full,
 *           lower the remembered inode
 *   alloc   take a free data block from the superblock's free block list;
 *           when the list's last entry is taken, that block holds the next
 *           part of the list, which is copied into the superblock
 *   free    put a block back in the list; when the superblock's list is
 *           full, write it into the freed block and start a new list
 *
 * Because all of this lives in the volume's pages, a checkpoint of the
 * volume is a checkpoint of the whole filesystem, recovery brings it all
 * back after a crash, and duplication can share a file's data blocks.
 *
 * Every operation is atomic: if it fails part-way (for example the disk
 * is full), the volume goes back to the tree it had before.
 *
 * Paths are absolute ("/docs/notes.txt"). Errors are negative errno
 * values: -ENOENT, -EEXIST, -ENOTDIR, -EISDIR, -ENOTEMPTY, -ENOSPC,
 * -EFBIG, -ENAMETOOLONG, -EINVAL, -EPERM, -EROFS, -EIO.
 */
#ifndef FS_H
#define FS_H

#include <stdint.h>

#include "volume.h"

#define FS_INODES 64                         /* size of the inode table          */
#define FS_DIRECT 12                         /* direct block pointers per inode  */
#define FS_NICINOD 8                         /* free inode numbers in superblock */
#define FS_NICFREE 12                        /* free block numbers in superblock */
#define FS_NAME_MAX 27                       /* longest name in a directory      */
#define FS_PATH_MAX 255                      /* longest path                     */
#define FS_MAX_FILE (FS_DIRECT * BLOCK_SIZE) /* 1536 bytes                       */
#define FS_ROOT_INO 1                        /* inode of "/"                     */

typedef enum { FS_FREE = 0, FS_FILE = 1, FS_DIR = 2 } FsType;

typedef struct Fs Fs;

typedef struct {
    uint32_t ino;    /* inode number                       */
    FsType type;     /* FS_FILE or FS_DIR                  */
    uint32_t nlink;  /* directory entries that point here  */
    uint32_t size;   /* bytes                              */
    uint32_t blocks; /* data blocks the file uses          */
} FsStat;

/* A view of the superblock, to watch the allocation algorithms work. */
typedef struct {
    uint32_t inodes_total, inodes_free;
    uint32_t blocks_total, blocks_free;         /* data blocks              */
    uint32_t ninode;                            /* free inode list cache:   */
    uint32_t inode_list[FS_NICINOD];            /*   [ninode - 1] goes next */
    uint32_t remembered_inode;                  /* where ialloc searches    */
    uint32_t nfree;                             /* free block list cache:   */
    uint32_t free_list[FS_NICFREE];             /*   [0] = next list block  */
} FsInfo;

/* ------------------------------------------------- make, open, close */

/* mkfs: write an empty filesystem (just "/") into the live volume. */
int fs_format(Volume *v);

/* Open the filesystem in the live tree. NULL if there is none. */
Fs *fs_open(Volume *v);

/* Open the filesystem as it is in another tree, for example a
 * checkpoint's root. Read-only: changes return -EROFS. The tree must
 * stay alive while it is open. */
Fs *fs_open_tree(Volume *v, uint32_t root);

void fs_close(Fs *fs);

/* ----------------------------------------------------- operations */

int fs_create(Fs *fs, const char *path);                        /* empty file */
int fs_mkdir(Fs *fs, const char *path);
int fs_unlink(Fs *fs, const char *path);                        /* a file     */
int fs_rmdir(Fs *fs, const char *path);                         /* empty dir  */
int fs_link(Fs *fs, const char *existing, const char *newpath); /* hard link  */

/* Write len bytes at offset; the file grows as needed. Returns the bytes
 * written or a negative error. A gap left before offset reads as zeros. */
int fs_write(Fs *fs, const char *path, uint32_t offset, const void *buf, uint32_t len);

/* Read up to len bytes from offset. Returns the bytes read (0 at the end
 * of the file) or a negative error. */
int fs_read(Fs *fs, const char *path, uint32_t offset, void *buf, uint32_t len);

/* Set the file's size: shrinking frees blocks past the end, growing adds
 * a gap that reads as zeros. */
int fs_truncate(Fs *fs, const char *path, uint32_t size);

int fs_stat(Fs *fs, const char *path, FsStat *st);

/* Call fn for every entry of a directory, "." and ".." included. */
typedef void (*fs_dirent_fn)(void *arg, const char *name, const FsStat *st);
int fs_readdir(Fs *fs, const char *path, fs_dirent_fn fn, void *arg);

int fs_info(Fs *fs, FsInfo *info);

/* ------------------------------------------------- for features */

/* bmap: the block (volume page) that holds block `index` of a file, 0 if
 * none. With alloc set, a missing block is allocated first (alloc). */
int fs_bmap(Fs *fs, const char *path, uint32_t index, int alloc, uint32_t *block);

Volume *fs_volume(Fs *fs);

/* fsck: check every structure from scratch (directory tree and link
 * counts, the free inode list and remembered inode, the chained free
 * block list, totals). Returns the number of problems; 0 = clean. */
int fs_check(Fs *fs);

#endif
