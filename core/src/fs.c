/*
 * fs.c - mkfs and the file operations, built on the algorithms in
 * fs_block.c (alloc, free), fs_inode.c (ialloc, ifree, bmap) and
 * fs_dir.c (namei).
 *
 * Every operation that changes something runs as one transaction:
 *   1. hold a reference to the tree as it is now;
 *   2. load the superblock into memory;
 *   3. do the work;
 *   4. write the superblock back if it changed;
 *   5. on any failure, make the held tree the live one again (undo all);
 *   6. let the held tree go.
 * Copy-on-write makes step 1 cost nothing: the held tree shares every
 * block the operation does not change.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "fs_impl.h"

_Static_assert(sizeof(Super) <= BLOCK_SIZE, "the superblock fits in a block");
_Static_assert(sizeof(Dinode) == 64, "an inode is 64 bytes");
_Static_assert(sizeof(Dirent) == 32, "a directory entry is 32 bytes");
_Static_assert(sizeof(FreeBlock) <= BLOCK_SIZE, "a free-list block fits");
_Static_assert(FS_INODES % (BLOCK_SIZE / 64) == 0, "whole inode blocks");

/* --------------------------------------------------------- transactions */

static int begin(Fs *fs, uint32_t *saved)
{
    if (fs->root)
        return -EROFS;
    *saved = vol_live_root(fs->v);
    cowtree_share(vol_tree(fs->v), *saved);
    super_load(fs);
    return 0;
}

static int end(Fs *fs, uint32_t saved, int result)
{
    if (result >= 0 && fs->sb_dirty) {
        int r = super_store(fs);
        if (r < 0)
            result = r;
    }
    if (result < 0)
        vol_set_live_root(fs->v, saved); /* undo everything */
    cowtree_drop(vol_tree(fs->v), saved);
    return result;
}

#define ATOMIC(fs, call)                       \
    do {                                       \
        uint32_t saved_;                       \
        int b_ = begin((fs), &saved_);         \
        if (b_ < 0)                            \
            return b_;                         \
        return end((fs), saved_, (call));      \
    } while (0)

/* ------------------------------------------------------------------ mkfs */

static int do_format(Fs *fs)
{
    Super *sb = &fs->sb;
    Dirent ents[DPB];
    Dinode root;
    uint32_t ino, bno;

    for (uint32_t p = 0; p < NBLOCKS; p++)
        TRY(bclear(fs, p)); /* all zeros: every inode in the table is free */

    memset(sb, 0, sizeof *sb);
    sb->magic = FS_MAGIC;
    sb->ninodes = FS_INODES;
    sb->nblocks = NBLOCKS;
    sb->data_start = DATA_START;
    sb->root_ino = FS_ROOT_INO;
    sb->tinode = FS_INODES - 1; /* inode 0 is never used */
    sb->rinode = 1;             /* the first ialloc searches from inode 1 */
    fs->sb_dirty = 1;

    /* The free block list: free every data block, highest first, so the
     * lowest numbers are handed out first. */
    for (uint32_t b = NBLOCKS - 1; b >= DATA_START; b--)
        TRY(bfree(fs, b));

    /* "/": the first inode (1) and the first data block. */
    TRY(ialloc(fs, FS_DIR, &ino));
    if (ino != FS_ROOT_INO)
        return -EIO;
    iget(fs, ino, &root);
    TRY(bmap(fs, &root, 0, 1, &bno));
    memset(ents, 0, sizeof ents);
    ents[0].ino = FS_ROOT_INO;
    strcpy(ents[0].name, ".");
    ents[1].ino = FS_ROOT_INO;
    strcpy(ents[1].name, ".."); /* in "/", ".." is "/" itself */
    TRY(bwrite(fs, bno, ents));
    root.nlink = 2;
    root.size = BLOCK_SIZE;
    return iupdate(fs, ino, &root);
}

int fs_format(Volume *v)
{
    Fs tmp = { .v = v, .root = 0 };
    ATOMIC(&tmp, do_format(&tmp));
}

/* ------------------------------------------------------ open and close */

static Fs *open_at(Volume *v, uint32_t root)
{
    Fs probe = { .v = v, .root = root };
    super_load(&probe);
    if (probe.sb.magic != FS_MAGIC || probe.sb.ninodes != FS_INODES ||
        probe.sb.data_start != DATA_START || probe.sb.nblocks != NBLOCKS)
        return NULL;
    Fs *fs = malloc(sizeof *fs);
    if (fs)
        *fs = probe;
    return fs;
}

Fs *fs_open(Volume *v)
{
    return open_at(v, 0);
}

Fs *fs_open_tree(Volume *v, uint32_t root)
{
    return root ? open_at(v, root) : NULL;
}

void fs_close(Fs *fs)
{
    free(fs);
}

Volume *fs_volume(Fs *fs)
{
    return fs->v;
}

/* -------------------------------------------- create, mkdir, link */

static int make_node(Fs *fs, const char *path, FsType type)
{
    char name[FS_NAME_MAX + 1];
    uint32_t parent, ino;
    Dinode pd, ip;

    TRY(nameiparent(fs, path, &parent, name));
    iget(fs, parent, &pd);
    if (dirlookup(fs, &pd, name, &ino) == 0)
        return -EEXIST;

    TRY(ialloc(fs, type, &ino));         /* 1. a free inode        */
    TRY(dirlink(fs, parent, name, ino)); /* 2. its name in the parent */
    iget(fs, ino, &ip);
    ip.nlink = 1;
    TRY(iupdate(fs, ino, &ip));
    if (type == FS_DIR) {                /* 3. a directory's own entries */
        TRY(dirlink(fs, ino, ".", ino));
        TRY(dirlink(fs, ino, "..", parent));
        iget(fs, ino, &ip);
        ip.nlink = 2;                    /* its name + its own "." */
        TRY(iupdate(fs, ino, &ip));
        iget(fs, parent, &pd);
        pd.nlink++;                      /* the new ".." points at the parent */
        TRY(iupdate(fs, parent, &pd));
    }
    return 0;
}

int fs_create(Fs *fs, const char *path)
{
    ATOMIC(fs, make_node(fs, path, FS_FILE));
}

int fs_mkdir(Fs *fs, const char *path)
{
    ATOMIC(fs, make_node(fs, path, FS_DIR));
}

static int do_link(Fs *fs, const char *existing, const char *newpath)
{
    char name[FS_NAME_MAX + 1];
    uint32_t ino, parent, other;
    Dinode ip, pd;

    TRY(namei(fs, existing, &ino));
    iget(fs, ino, &ip);
    if (ip.type == FS_DIR)
        return -EPERM; /* no hard links to directories, as in Unix */
    TRY(nameiparent(fs, newpath, &parent, name));
    iget(fs, parent, &pd);
    if (dirlookup(fs, &pd, name, &other) == 0)
        return -EEXIST;
    TRY(dirlink(fs, parent, name, ino));
    iget(fs, ino, &ip);
    ip.nlink++;
    return iupdate(fs, ino, &ip);
}

int fs_link(Fs *fs, const char *existing, const char *newpath)
{
    ATOMIC(fs, do_link(fs, existing, newpath));
}

/* ------------------------------------------------------ unlink, rmdir */

static int do_unlink(Fs *fs, const char *path)
{
    char name[FS_NAME_MAX + 1];
    uint32_t parent, ino;
    Dinode pd, ip;

    TRY(nameiparent(fs, path, &parent, name));
    iget(fs, parent, &pd);
    TRY(dirlookup(fs, &pd, name, &ino));
    iget(fs, ino, &ip);
    if (ip.type == FS_DIR)
        return -EISDIR;
    TRY(dirunlink(fs, parent, name)); /* 1. remove the name          */
    ip.nlink--;
    if (ip.nlink > 0)
        return iupdate(fs, ino, &ip); /* other hard links remain     */
    return iremove(fs, ino, &ip);     /* 2. last link: free blocks + inode */
}

int fs_unlink(Fs *fs, const char *path)
{
    ATOMIC(fs, do_unlink(fs, path));
}

static int do_rmdir(Fs *fs, const char *path)
{
    char name[FS_NAME_MAX + 1];
    uint32_t parent, ino;
    Dinode pd, ip;

    TRY(nameiparent(fs, path, &parent, name));
    iget(fs, parent, &pd);
    TRY(dirlookup(fs, &pd, name, &ino));
    iget(fs, ino, &ip);
    if (ip.type != FS_DIR)
        return -ENOTDIR;
    if (!dirempty(fs, &ip))
        return -ENOTEMPTY;
    TRY(dirunlink(fs, parent, name));
    iget(fs, parent, &pd);
    pd.nlink--; /* its ".." no longer points at the parent */
    TRY(iupdate(fs, parent, &pd));
    return iremove(fs, ino, &ip);
}

int fs_rmdir(Fs *fs, const char *path)
{
    ATOMIC(fs, do_rmdir(fs, path));
}

/* ------------------------------------------------- read, write, truncate */

static int do_write(Fs *fs, const char *path, uint32_t off, const uint8_t *buf, uint32_t len)
{
    uint8_t block[BLOCK_SIZE];
    uint32_t ino;
    Dinode ip;

    TRY(namei(fs, path, &ino));
    iget(fs, ino, &ip);
    if (ip.type == FS_DIR)
        return -EISDIR;
    if (off > FS_MAX_FILE || len > FS_MAX_FILE - off)
        return -EFBIG;

    for (uint32_t pos = off, end_ = off + len; pos < end_;) {
        uint32_t idx = pos / BLOCK_SIZE, at = pos % BLOCK_SIZE, bno;
        uint32_t n = BLOCK_SIZE - at < end_ - pos ? BLOCK_SIZE - at : end_ - pos;
        TRY(bmap(fs, &ip, idx, 1, &bno)); /* alloc a block if the file has none here */
        bread(fs, bno, block);
        memcpy(block + at, buf + (pos - off), n);
        TRY(bwrite(fs, bno, block));      /* copy-on-write in the volume */
        pos += n;
    }
    if (off + len > ip.size)
        ip.size = off + len;
    TRY(iupdate(fs, ino, &ip));
    return (int)len;
}

int fs_write(Fs *fs, const char *path, uint32_t offset, const void *buf, uint32_t len)
{
    ATOMIC(fs, do_write(fs, path, offset, buf, len));
}

int fs_read(Fs *fs, const char *path, uint32_t off, void *out, uint32_t len)
{
    uint8_t block[BLOCK_SIZE];
    uint8_t *dst = out;
    uint32_t ino;
    Dinode ip;

    TRY(namei(fs, path, &ino));
    iget(fs, ino, &ip);
    if (ip.type == FS_DIR)
        return -EISDIR;
    if (off >= ip.size)
        return 0;
    if (len > ip.size - off)
        len = ip.size - off;

    for (uint32_t done = 0; done < len;) {
        uint32_t pos = off + done, idx = pos / BLOCK_SIZE, at = pos % BLOCK_SIZE, bno;
        uint32_t n = BLOCK_SIZE - at < len - done ? BLOCK_SIZE - at : len - done;
        TRY(bmap(fs, &ip, idx, 0, &bno));
        if (bno)
            bread(fs, bno, block);
        else
            memset(block, 0, sizeof block); /* a gap in the file */
        memcpy(dst + done, block + at, n);
        done += n;
    }
    return (int)len;
}

static int do_truncate(Fs *fs, const char *path, uint32_t size)
{
    uint8_t block[BLOCK_SIZE];
    uint32_t ino;
    Dinode ip;

    TRY(namei(fs, path, &ino));
    iget(fs, ino, &ip);
    if (ip.type == FS_DIR)
        return -EISDIR;
    if (size > FS_MAX_FILE)
        return -EFBIG;
    if (size < ip.size) {
        TRY(itrunc(fs, &ip, (size + BLOCK_SIZE - 1) / BLOCK_SIZE)); /* free the rest */
        uint32_t last = size % BLOCK_SIZE ? ip.direct[size / BLOCK_SIZE] : 0;
        if (last) {
            /* Zero the tail of the last block, so growing later reads zeros. */
            bread(fs, last, block);
            memset(block + size % BLOCK_SIZE, 0, BLOCK_SIZE - size % BLOCK_SIZE);
            TRY(bwrite(fs, last, block));
        }
    }
    ip.size = size;
    return iupdate(fs, ino, &ip);
}

int fs_truncate(Fs *fs, const char *path, uint32_t size)
{
    ATOMIC(fs, do_truncate(fs, path, size));
}

/* --------------------------------------------------- stat, readdir, info */

static void fill_stat(uint32_t ino, const Dinode *ip, FsStat *st)
{
    st->ino = ino;
    st->type = (FsType)ip->type;
    st->nlink = ip->nlink;
    st->size = ip->size;
    st->blocks = 0;
    for (int i = 0; i < FS_DIRECT; i++)
        if (ip->direct[i])
            st->blocks++;
}

int fs_stat(Fs *fs, const char *path, FsStat *st)
{
    uint32_t ino;
    Dinode ip;
    TRY(namei(fs, path, &ino));
    iget(fs, ino, &ip);
    fill_stat(ino, &ip, st);
    return 0;
}

int fs_readdir(Fs *fs, const char *path, fs_dirent_fn fn, void *arg)
{
    Dirent ents[DPB];
    uint32_t ino;
    Dinode dir, ip;
    FsStat st;

    TRY(namei(fs, path, &ino));
    iget(fs, ino, &dir);
    if (dir.type != FS_DIR)
        return -ENOTDIR;
    for (uint32_t i = 0; i < dir.size / BLOCK_SIZE; i++) {
        bread(fs, dir.direct[i], ents);
        for (uint32_t j = 0; j < DPB; j++)
            if (ents[j].ino) {
                iget(fs, ents[j].ino, &ip);
                fill_stat(ents[j].ino, &ip, &st);
                fn(arg, ents[j].name, &st);
            }
    }
    return 0;
}

int fs_info(Fs *fs, FsInfo *info)
{
    super_load(fs);
    const Super *sb = &fs->sb;
    memset(info, 0, sizeof *info);
    info->inodes_total = sb->ninodes - 1; /* inode 0 is never used */
    info->inodes_free = sb->tinode;
    info->blocks_total = sb->nblocks - sb->data_start;
    info->blocks_free = sb->tfree;
    info->ninode = sb->ninode;
    memcpy(info->inode_list, sb->inode, sizeof info->inode_list);
    info->remembered_inode = sb->rinode;
    info->nfree = sb->nfree;
    memcpy(info->free_list, sb->free, sizeof info->free_list);
    return 0;
}

/* ------------------------------------------------------------------ bmap */

static int do_bmap(Fs *fs, const char *path, uint32_t index, int alloc, uint32_t *bno)
{
    uint32_t ino;
    Dinode ip;

    TRY(namei(fs, path, &ino));
    iget(fs, ino, &ip);
    if (ip.type == FS_DIR)
        return -EISDIR;
    uint32_t before = index < FS_DIRECT ? ip.direct[index] : 0;
    TRY(bmap(fs, &ip, index, alloc, bno));
    if (*bno != before)
        TRY(iupdate(fs, ino, &ip));
    return 0;
}

int fs_bmap(Fs *fs, const char *path, uint32_t index, int alloc, uint32_t *bno)
{
    if (!alloc)
        return do_bmap(fs, path, index, 0, bno);
    ATOMIC(fs, do_bmap(fs, path, index, 1, bno));
}
