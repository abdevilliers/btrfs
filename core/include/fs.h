/*
 * fs.h - the public API of the filesystem: a model of btrfs.
 *
 * Two handles:
 *
 *   Btrfs  a mounted filesystem (the kernel's fs_info): the superblock,
 *          every tree, the block groups, the running transaction.
 *   Fs     one subvolume of it, opened like a mount (mount -o subvol=):
 *          paths are relative to that subvolume's root directory.
 *
 * How it is stored (see core/FILESYSTEM.md):
 *
 *   - Every piece of metadata is an item in a copy-on-write B-tree. No
 *     tree block is ever changed once written: a change writes new copies
 *     of the blocks on the path, up to a new root.
 *   - The root tree holds the root of every other tree: the extent tree
 *     (allocated space and its back-references), the csum tree (data
 *     checksums), the free space tree, the uuid tree, and one FS tree per
 *     subvolume (inodes, directory entries, file extents).
 *   - Changes collect in a running transaction. btrfs_commit writes the
 *     new blocks, then the superblock: until then, a crash brings back the
 *     previous commit.
 *
 * Errors are negative errno values: -ENOENT, -EEXIST, -ENOTDIR, -EISDIR,
 * -ENOTEMPTY, -ENOSPC, -EFBIG, -ENAMETOOLONG, -EINVAL, -EPERM, -EROFS,
 * -EIO, -EXDEV, -ESTALE, -EBUSY.
 */
#ifndef FS_H
#define FS_H

#include <stdint.h>

#include "btrfs_tree.h"
#include "disk.h"

typedef struct Btrfs Btrfs;
typedef struct Fs Fs;

#define FS_NAME_MAX BTRFS_NAME_LEN /* longest name in a directory */
#define FS_PATH_MAX 4095           /* longest path                */
#define FS_ROOT_INO BTRFS_FIRST_FREE_OBJECTID /* "/" of every subvolume */

/* ------------------------------------------------------ mkfs and mount */

typedef struct {
    uint32_t nodesize;  /* tree block size: 4096 .. 65536 (default 16384) */
    const char *label;
} BtrfsMkfsOpts;

/* mkfs.btrfs: an empty filesystem whose top-level subvolume (5) holds an
 * empty "/". The disk must hold at least 2 MiB. */
int btrfs_mkfs(Disk *disk, const BtrfsMkfsOpts *opts);

typedef struct {
    int usebackuproot; /* -o usebackuproot: if the tree root is damaged,
                          try the backup roots in the superblock        */
    int read_only;     /* -o ro                                         */
} BtrfsMountOpts;

/* Mount the filesystem on `disk` as of its last commit. -ENOENT if the
 * disk holds no btrfs, -EIO if the superblock or a tree root is damaged. */
int btrfs_mount(Disk *disk, const BtrfsMountOpts *opts, Btrfs **out);

/* Commit, then forget the filesystem. The disk is not closed. */
int btrfs_unmount(Btrfs *b);

/* Forget the filesystem WITHOUT committing: a power cut. Everything since
 * the last commit is lost; the disk keeps only what was committed. */
void btrfs_crash(Btrfs *b);

/* Commit the running transaction (sync). After this returns, a crash
 * brings back exactly this state. */
int btrfs_commit(Btrfs *b);

uint64_t btrfs_generation(const Btrfs *b); /* the last committed transid */
int btrfs_is_read_only(const Btrfs *b);    /* after an abort, or -o ro */
Disk *btrfs_disk(Btrfs *b);

/* btrfs rescue super-recover: if the primary superblock is damaged, copy
 * the newest good mirror over it. 1 = repaired, 0 = nothing to do,
 * -ENOENT = no good copy exists. */
int btrfs_super_recover(Disk *disk);

/* The generation of the primary superblock on the disk; 0 if there is no
 * valid one. */
uint64_t btrfs_disk_generation(Disk *disk);

/* btrfs check: read the committed filesystem from the disk and check
 * every structure: tree block checksums, generations and key order, every
 * extent's reference count and back-references against the trees, block
 * group usage, the free space tree, data checksums, and each FS tree's
 * inodes, directory entries and file extents. Returns the number of
 * problems (0 = clean). verbose = print each problem. */
int btrfs_check(Disk *disk, int verbose);

/* -------------------------------------------------------- subvolumes */

Fs *fs_open(Btrfs *b);                       /* the default subvolume      */
Fs *fs_open_subvol(Btrfs *b, uint64_t id);   /* any subvolume, by id       */
void fs_close(Fs *fs);
Btrfs *fs_btrfs(Fs *fs);
uint64_t fs_subvol_id(const Fs *fs);
int fs_is_read_only(Fs *fs);

/* btrfs subvolume create PATH: an empty subvolume at PATH. */
int fs_subvol_create(Fs *fs, const char *path);

/* btrfs subvolume snapshot [-r] SRC DST: SRC is the root of a subvolume
 * ("/" for this one). The snapshot is made while the running transaction
 * commits, and this call commits it, as btrfs does. *id = the new
 * subvolume's id (may be NULL). */
int fs_subvol_snapshot(Fs *fs, const char *src, const char *dst, int readonly, uint64_t *id);

/* btrfs subvolume delete PATH: its directory entry goes now; its blocks are
 * freed by the cleaner after the next commit. -EPERM for the default or
 * the top-level subvolume, -ENOTEMPTY if it holds other subvolumes. */
int fs_subvol_delete(Fs *fs, const char *path);

typedef struct {
    uint64_t id;
    uint64_t parent_id;  /* the subvolume holding its directory entry (0 for 5) */
    uint64_t dirid;      /* the directory there                                 */
    char name[FS_NAME_MAX + 1];
    int read_only;
    uint64_t otransid;   /* the transaction that created it                     */
    uint64_t generation; /* the last transaction that changed it                */
    uint8_t uuid[BTRFS_UUID_SIZE];
    uint8_t parent_uuid[BTRFS_UUID_SIZE]; /* the source of a snapshot           */
} FsSubvolInfo;

/* btrfs subvolume list: every live subvolume (not 5), in id order. */
typedef void (*fs_subvol_fn)(void *arg, const FsSubvolInfo *info);
int fs_subvol_list(Btrfs *b, fs_subvol_fn fn, void *arg);
int fs_subvol_info(Btrfs *b, uint64_t id, FsSubvolInfo *info);

/* The id of the subvolume whose root directory is PATH. -EINVAL if PATH
 * is a directory but not a subvolume root. */
int fs_subvol_lookup(Fs *fs, const char *path, uint64_t *id);

/* btrfs subvolume get-default / set-default (what fs_open opens). */
uint64_t fs_get_default_subvol(Btrfs *b);
int fs_set_default_subvol(Btrfs *b, uint64_t id);

/* ------------------------------------------------------ files and dirs */

typedef enum { FS_FILE = 1, FS_DIR = 2 } FsType;

typedef struct {
    uint64_t ino;     /* inode number within its subvolume       */
    uint64_t subvol;  /* the subvolume (FS tree) holding it       */
    FsType type;
    uint32_t nlink;
    uint64_t size;    /* bytes                                    */
    uint64_t nbytes;  /* bytes of extents (st_blocks * 512)       */
    uint64_t generation; /* transid that created the inode         */
    uint64_t transid;    /* transid that last changed it           */
} FsStat;

int fs_create(Fs *fs, const char *path);                        /* empty file */
int fs_mkdir(Fs *fs, const char *path);
int fs_unlink(Fs *fs, const char *path);                        /* a file     */
int fs_rmdir(Fs *fs, const char *path);   /* an empty dir, or an empty subvolume */
int fs_link(Fs *fs, const char *existing, const char *newpath); /* hard link  */

/* Write len bytes at offset; the file grows as needed. Returns the bytes
 * written or a negative error. A gap left before offset reads as zeros. */
int64_t fs_write(Fs *fs, const char *path, uint64_t offset, const void *buf, uint64_t len);

/* Read up to len bytes from offset. Returns the bytes read (0 at the end
 * of the file) or a negative error (-EIO: a data checksum failed). */
int64_t fs_read(Fs *fs, const char *path, uint64_t offset, void *buf, uint64_t len);

int fs_truncate(Fs *fs, const char *path, uint64_t size);
int fs_stat(Fs *fs, const char *path, FsStat *st);

/* Every entry of a directory, "." and ".." first, then in DIR_INDEX order. */
typedef void (*fs_dirent_fn)(void *arg, const char *name, const FsStat *st);
int fs_readdir(Fs *fs, const char *path, fs_dirent_fn fn, void *arg);

/* ioctl FICLONERANGE: make [dst_off, dst_off + len) of dst share the data
 * extents of [src_off, src_off + len) of src: no data is copied. Offsets
 * and len must be multiples of the sector size, except that a range may
 * end at src's end of file. len = 0 means "to src's end of file". The two
 * files may be in different subvolumes. */
int fs_clone_range(Fs *fs, const char *src, uint64_t src_off, const char *dst, uint64_t dst_off,
                   uint64_t len);

/* ioctl FIEMAP: the extents of a file, in file order. */
#define FIEMAP_EXTENT_INLINE 0x200u /* data is in the tree, not an extent      */
#define FIEMAP_EXTENT_SHARED 0x2000u /* another file or snapshot uses it too   */
typedef struct {
    uint64_t logical;  /* offset in the file   */
    uint64_t physical; /* byte on the disk (0 for inline) */
    uint64_t length;
    uint32_t flags;
    uint64_t extent_bytenr; /* the data extent it is part of (its extent item's key) */
} FsExtent;
typedef void (*fs_extent_fn)(void *arg, const FsExtent *e);
int fs_fiemap(Fs *fs, const char *path, fs_extent_fn fn, void *arg);

/* ------------------------------------------------------- statfs, df */

typedef struct {
    uint64_t total_bytes;     /* the device                              */
    uint64_t bytes_used;      /* in extents (superblock bytes_used)      */
    uint64_t data_total, data_used;         /* data block groups        */
    uint64_t metadata_total, metadata_used; /* metadata block groups    */
    uint64_t unallocated;     /* not in any block group yet              */
    uint64_t free_estimate;   /* data_total - data_used + unallocated    */
    uint32_t block_groups;
    uint32_t sectorsize, nodesize;
} FsSpace;

int fs_space(Btrfs *b, FsSpace *out); /* btrfs filesystem df / usage */

/* ------------------------------------- reading the trees (inspect-internal) */

/* BTRFS_IOC_TREE_SEARCH: every item of tree `tree_id` (1 = root tree,
 * 2 = extent tree, 5 or a subvolume id = an FS tree, ...) with
 * min <= key <= max, in key order, as of the running transaction. fn
 * returns nonzero to stop. */
typedef int (*fs_item_fn)(void *arg, const BtrfsKey *key, const void *data, uint32_t len);
int btrfs_tree_search(Btrfs *b, uint64_t tree_id, const BtrfsKey *min, const BtrfsKey *max,
                      fs_item_fn fn, void *arg);

/* btrfs inspect-internal dump-tree -t ID / dump-super: print to stdout. */
int btrfs_dump_tree(Btrfs *b, uint64_t tree_id);
void btrfs_dump_super(Btrfs *b);

/* Counters, for demos: tree blocks and data sectors written to the disk,
 * transactions committed. */
typedef struct {
    uint64_t tree_blocks_written;
    uint64_t data_sectors_written;
    uint64_t commits;
    uint64_t cow_copies; /* tree blocks copied by copy-on-write */
} BtrfsStats;
void btrfs_stats(Btrfs *b, BtrfsStats *out);

#endif
