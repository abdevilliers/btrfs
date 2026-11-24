/*
 * fs_check.c - fsck: check every structure of the filesystem from
 * scratch. Returns the number of problems found; 0 means clean.
 *
 *   1. The directory tree: entries point at used inodes, "." and ".."
 *      are right, no directory is reached twice.
 *   2. Link counts: entries pointing at each inode == its nlink.
 *   3. The free inode cache: entries are free and unique; every free
 *      inode not in it is at or above the remembered inode; the free
 *      inode total is right.
 *   4. The free block list: following the chain finds every data block
 *      that no file uses, exactly once; free blocks hold no data except
 *      the ones holding the list; the free block total is right.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "fs_impl.h"

enum { UNSEEN = 0, USED = 1, FREE_BLOCK = 2 };

typedef struct {
    Fs *fs;
    uint32_t links[FS_INODES];
    uint8_t seen_dir[FS_INODES];
    uint8_t block[NBLOCKS]; /* UNSEEN, USED or FREE_BLOCK */
    int errors;
} Check;

static void mark(Check *c, uint32_t bno, uint8_t how)
{
    if (bno < DATA_START || bno >= NBLOCKS || c->block[bno] != UNSEEN)
        c->errors++; /* outside the data area, or counted twice */
    else
        c->block[bno] = how;
}

static void mark_file(Check *c, const Dinode *ip)
{
    for (int i = 0; i < FS_DIRECT; i++)
        if (ip->direct[i])
            mark(c, ip->direct[i], USED);
}

static void check_dir(Check *c, uint32_t dino, uint32_t parent, int depth)
{
    Dirent ents[DPB];
    Dinode dir, ip;
    int dot = 0, dotdot = 0;

    if (depth > 64 || c->seen_dir[dino]) {
        c->errors++;
        return;
    }
    c->seen_dir[dino] = 1;
    iget(c->fs, dino, &dir);
    mark_file(c, &dir);
    for (uint32_t i = 0; i < dir.size / BLOCK_SIZE; i++) {
        if (i >= FS_DIRECT || !dir.direct[i]) {
            c->errors++;
            continue;
        }
        bread(c->fs, dir.direct[i], ents);
        for (uint32_t j = 0; j < DPB; j++) {
            uint32_t ino = ents[j].ino;
            if (!ino)
                continue;
            if (ino >= FS_INODES) {
                c->errors++;
                continue;
            }
            c->links[ino]++;
            iget(c->fs, ino, &ip);
            if (strcmp(ents[j].name, ".") == 0) {
                dot++;
                c->errors += ino != dino;
            } else if (strcmp(ents[j].name, "..") == 0) {
                dotdot++;
                c->errors += ino != parent;
            } else if (ip.type == FS_DIR) {
                check_dir(c, ino, dino, depth + 1);
            } else if (ip.type == FS_FILE) {
                if (c->links[ino] == 1)
                    mark_file(c, &ip); /* a hard-linked file counts once */
                c->errors += ip.size > FS_MAX_FILE;
            } else {
                c->errors++; /* an entry pointing at a free inode */
            }
        }
    }
    c->errors += dot != 1 || dotdot != 1;
}

int fs_check(Fs *fs)
{
    Check *c = calloc(1, sizeof *c);
    uint8_t in_cache[FS_INODES] = { 0 };
    Super sb;
    Dinode ip;
    int errors;

    if (!c)
        return -ENOMEM;
    c->fs = fs;
    super_load(fs);
    sb = fs->sb;
    if (sb.magic != FS_MAGIC || sb.nblocks != NBLOCKS || sb.data_start != DATA_START) {
        free(c);
        return 1;
    }

    /* 1 and 2: the directory tree and link counts. */
    check_dir(c, FS_ROOT_INO, FS_ROOT_INO, 0);

    /* 3: the free inode cache. */
    if (sb.ninode > FS_NICINOD) {
        c->errors++;
        sb.ninode = 0;
    }
    for (uint32_t k = 0; k < sb.ninode; k++) {
        uint32_t ino = sb.inode[k];
        if (ino == 0 || ino >= FS_INODES || in_cache[ino]) {
            c->errors++;
            continue;
        }
        in_cache[ino] = 1;
        iget(fs, ino, &ip);
        c->errors += ip.type != FS_FREE;
    }
    uint32_t free_inodes = 0;
    for (uint32_t ino = 1; ino < FS_INODES; ino++) {
        iget(fs, ino, &ip);
        if (ip.type == FS_FREE) {
            free_inodes++;
            c->errors += c->links[ino] != 0;
            c->errors += !in_cache[ino] && ino < sb.rinode; /* ialloc would miss it */
        } else {
            c->errors += c->links[ino] == 0 || c->links[ino] != ip.nlink;
        }
    }
    c->errors += free_inodes != sb.tinode;

    /* 4: follow the chained free block list. */
    uint32_t nfree = sb.nfree, list[FS_NICFREE], free_blocks = 0;
    memcpy(list, sb.free, sizeof list);
    for (uint32_t hops = 0; nfree > 0; hops++) {
        if (nfree > FS_NICFREE || hops > NBLOCKS) {
            c->errors++;
            break;
        }
        for (uint32_t i = nfree - 1; i >= 1; i--) {
            mark(c, list[i], FREE_BLOCK);
            free_blocks++;
            if (cowtree_get(vol_tree(fs->v), fs->root ? fs->root : vol_live_root(fs->v), list[i]))
                c->errors++; /* a free block still holds data */
        }
        uint32_t link = list[0];
        if (link == 0)
            break; /* the end of the list */
        mark(c, link, FREE_BLOCK);
        free_blocks++;
        FreeBlock fb;
        uint8_t buf[BLOCK_SIZE];
        bread(fs, link, buf);
        memcpy(&fb, buf, sizeof fb);
        nfree = fb.nfree;
        memcpy(list, fb.free, sizeof list);
        c->errors += nfree == 0;
    }
    c->errors += free_blocks != sb.tfree;
    for (uint32_t b = DATA_START; b < NBLOCKS; b++)
        c->errors += c->block[b] == UNSEEN; /* neither used nor on the free list */

    errors = c->errors;
    free(c);
    return errors;
}
