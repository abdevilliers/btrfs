/*
 * fs_inode.c - inodes: reading and writing them, Bach's algorithms
 * "ialloc" and "ifree", freeing a file's blocks (itrunc), and bmap.
 *
 * The superblock caches a few free inode numbers:
 *
 *   superblock.inode[] = [ 8, 7, 6, 5 ]      ninode = 4
 *                                      ^ next to be allocated (5)
 *   superblock.rinode  = 8                   the "remembered inode"
 *
 * When the cache is empty, ialloc scans the inode table from the
 * remembered inode upwards for inodes whose type is 0 (free), fills the
 * cache, and remembers the highest one found. ifree puts an inode back in
 * the cache, or, if the cache is full, makes sure the next scan will find
 * it by lowering the remembered inode. So every free inode that is not in
 * the cache is at or above the remembered inode.
 */
#include <errno.h>
#include <string.h>

#include "fs_impl.h"

/* --------------------------------------------- reading and writing */

void iget(Fs *fs, uint32_t ino, Dinode *ip)
{
    uint8_t buf[BLOCK_SIZE];
    bread(fs, INODE_START + ino / IPB, buf);
    memcpy(ip, buf + (ino % IPB) * sizeof(Dinode), sizeof *ip);
}

int iupdate(Fs *fs, uint32_t ino, const Dinode *ip)
{
    uint8_t buf[BLOCK_SIZE];
    uint32_t bno = INODE_START + ino / IPB;
    bread(fs, bno, buf);
    memcpy(buf + (ino % IPB) * sizeof(Dinode), ip, sizeof *ip);
    return bwrite(fs, bno, buf);
}

/* --------------------------------------------------------------- ialloc */

/*
 * Bach, algorithm ialloc:
 *   while (not done) {
 *       if (inode list in super block is empty) {
 *           get remembered inode for free inode search;
 *           search disk for free inodes until super block full,
 *               or no more free inodes;
 *           if (no free inodes found on disk) return (no inode);
 *           set remembered inode for next free inode search;
 *       }
 *       get inode number from super block inode list;
 *       get inode (iget);
 *       if (inode not free after all) { write inode to disk; continue; }
 *       initialize inode;
 *       write inode to disk;
 *       decrement file system free inode count;
 *       return (inode);
 *   }
 */
int ialloc(Fs *fs, FsType type, uint32_t *out)
{
    Super *sb = &fs->sb;
    Dinode d;

    for (;;) {
        if (sb->ninode == 0) {
            uint32_t found[FS_NICINOD], n = 0;
            for (uint32_t ino = sb->rinode; ino < sb->ninodes && n < FS_NICINOD; ino++) {
                iget(fs, ino, &d);
                if (d.type == FS_FREE)
                    found[n++] = ino;
            }
            if (n == 0)
                return -ENOSPC; /* no free inode left */
            /* Store them so the lowest number is handed out first. */
            for (uint32_t k = 0; k < n; k++)
                sb->inode[k] = found[n - 1 - k];
            sb->ninode = n;
            sb->rinode = found[n - 1]; /* the highest one found */
            fs->sb_dirty = 1;
        }

        uint32_t ino = sb->inode[--sb->ninode];
        fs->sb_dirty = 1;
        iget(fs, ino, &d);
        if (d.type != FS_FREE)
            continue; /* not free after all: try the next one */

        memset(&d, 0, sizeof d);
        d.type = (uint16_t)type;
        TRY(iupdate(fs, ino, &d));
        sb->tinode--;
        *out = ino;
        return 0;
    }
}

/* ---------------------------------------------------------------- ifree */

/*
 * Bach, algorithm ifree:
 *   increment file system free inode count;
 *   if (inode list full) {
 *       if (inode number less than remembered inode for search)
 *           set remembered inode for search = input inode number;
 *   } else
 *       store inode number in inode list;
 *
 * The caller has already marked the inode free on disk (type 0).
 */
void ifree(Fs *fs, uint32_t ino)
{
    Super *sb = &fs->sb;

    sb->tinode++;
    if (sb->ninode == FS_NICINOD) {
        if (ino < sb->rinode)
            sb->rinode = ino;
    } else {
        sb->inode[sb->ninode++] = ino;
    }
    fs->sb_dirty = 1;
}

/* ------------------------------------------------------- freeing a file */

/* Free the file's blocks from index `from` onwards (Bach: free). */
int itrunc(Fs *fs, Dinode *ip, uint32_t from)
{
    for (uint32_t i = from; i < FS_DIRECT; i++)
        if (ip->direct[i]) {
            TRY(bfree(fs, ip->direct[i]));
            ip->direct[i] = 0;
        }
    return 0;
}

/* The last link is gone (Bach, iput): free the file's blocks, set the
 * file type to 0, write the inode, and free the inode (ifree). */
int iremove(Fs *fs, uint32_t ino, Dinode *ip)
{
    TRY(itrunc(fs, ip, 0));
    memset(ip, 0, sizeof *ip);
    ip->type = FS_FREE;
    TRY(iupdate(fs, ino, ip));
    ifree(fs, ino);
    return 0;
}

/* ----------------------------------------------------------------- bmap */

/* Which data block holds block `index` of the file; with alloc set, a
 * missing one is allocated (the caller writes the inode back). */
int bmap(Fs *fs, Dinode *ip, uint32_t index, int alloc, uint32_t *bno)
{
    if (index >= FS_DIRECT)
        return -EFBIG;
    if (!ip->direct[index] && alloc)
        TRY(balloc(fs, &ip->direct[index]));
    *bno = ip->direct[index];
    return 0;
}
