/*
 * fs_dir.c - directories and path lookup (namei).
 *
 * A directory is a file of type FS_DIR whose blocks hold 32-byte entries
 * { inode number, name }; inode number 0 marks an empty slot. Every
 * directory has "." (itself) and ".." (its parent).
 */
#include <errno.h>
#include <string.h>

#include "fs_impl.h"

static uint32_t dir_blocks(const Dinode *dp)
{
    return dp->size / BLOCK_SIZE;
}

int dirlookup(Fs *fs, const Dinode *dp, const char *name, uint32_t *ino)
{
    Dirent ents[DPB];
    for (uint32_t i = 0; i < dir_blocks(dp); i++) {
        bread(fs, dp->direct[i], ents);
        for (uint32_t j = 0; j < DPB; j++)
            if (ents[j].ino && strcmp(ents[j].name, name) == 0) {
                *ino = ents[j].ino;
                return 0;
            }
    }
    return -ENOENT;
}

/* Add { name, ino } in the first empty slot, or in a new block. */
int dirlink(Fs *fs, uint32_t dino, const char *name, uint32_t ino)
{
    Dirent ents[DPB];
    Dinode dir;

    iget(fs, dino, &dir);
    for (uint32_t i = 0; i < dir_blocks(&dir); i++) {
        bread(fs, dir.direct[i], ents);
        for (uint32_t j = 0; j < DPB; j++)
            if (ents[j].ino == 0) {
                memset(&ents[j], 0, sizeof ents[j]);
                ents[j].ino = ino;
                strcpy(ents[j].name, name);
                return bwrite(fs, dir.direct[i], ents);
            }
    }
    uint32_t n = dir_blocks(&dir), bno;
    if (n == FS_DIRECT)
        return -ENOSPC; /* the directory is full */
    TRY(bmap(fs, &dir, n, 1, &bno)); /* alloc: a new, zeroed block */
    memset(ents, 0, sizeof ents);
    ents[0].ino = ino;
    strcpy(ents[0].name, name);
    TRY(bwrite(fs, bno, ents));
    dir.size += BLOCK_SIZE;
    return iupdate(fs, dino, &dir);
}

int dirunlink(Fs *fs, uint32_t dino, const char *name)
{
    Dirent ents[DPB];
    Dinode dir;

    iget(fs, dino, &dir);
    for (uint32_t i = 0; i < dir_blocks(&dir); i++) {
        bread(fs, dir.direct[i], ents);
        for (uint32_t j = 0; j < DPB; j++)
            if (ents[j].ino && strcmp(ents[j].name, name) == 0) {
                memset(&ents[j], 0, sizeof ents[j]);
                return bwrite(fs, dir.direct[i], ents);
            }
    }
    return -ENOENT;
}

int dirempty(Fs *fs, const Dinode *dp)
{
    Dirent ents[DPB];
    for (uint32_t i = 0; i < dir_blocks(dp); i++) {
        bread(fs, dp->direct[i], ents);
        for (uint32_t j = 0; j < DPB; j++)
            if (ents[j].ino && strcmp(ents[j].name, ".") && strcmp(ents[j].name, ".."))
                return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- namei */

/* Copy the next name of *p into `name` and move past it.
 * 1 = a name was found, 0 = end of the path, or -ENAMETOOLONG. */
static int next_name(const char **p, char name[FS_NAME_MAX + 1])
{
    const char *s = *p;
    size_t n = 0;

    while (*s == '/')
        s++;
    if (!*s) {
        *p = s;
        return 0;
    }
    while (s[n] && s[n] != '/')
        n++;
    if (n > FS_NAME_MAX)
        return -ENAMETOOLONG;
    memcpy(name, s, n);
    name[n] = '\0';
    *p = s + n;
    return 1;
}

/* Bach, algorithm namei: start at "/", and for each name in the path,
 * search the current directory for it; its inode becomes the current one. */
int namei(Fs *fs, const char *path, uint32_t *out)
{
    char name[FS_NAME_MAX + 1];
    uint32_t ino = FS_ROOT_INO;
    int r;

    if (!path || path[0] != '/')
        return -EINVAL;
    if (strlen(path) > FS_PATH_MAX)
        return -ENAMETOOLONG;
    while ((r = next_name(&path, name)) == 1) {
        Dinode dir;
        iget(fs, ino, &dir);
        if (dir.type != FS_DIR)
            return -ENOTDIR;
        TRY(dirlookup(fs, &dir, name, &ino));
    }
    if (r < 0)
        return r;
    *out = ino;
    return 0;
}

/* "/a/b/c" -> the inode of "/a/b" and the last name "c". */
int nameiparent(Fs *fs, const char *path, uint32_t *parent, char name[FS_NAME_MAX + 1])
{
    char buf[FS_PATH_MAX + 1];
    Dinode dir;

    if (!path || path[0] != '/')
        return -EINVAL;
    if (strlen(path) > FS_PATH_MAX)
        return -ENAMETOOLONG;
    strcpy(buf, path);
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/')
        buf[--n] = '\0'; /* ignore trailing slashes */

    char *slash = strrchr(buf, '/');
    const char *last = slash + 1;
    if (!*last || strcmp(last, ".") == 0 || strcmp(last, "..") == 0)
        return -EINVAL; /* "/", "." and ".." cannot be created or removed */
    if (strlen(last) > FS_NAME_MAX)
        return -ENAMETOOLONG;
    strcpy(name, last);

    if (slash == buf) {
        *parent = FS_ROOT_INO;
    } else {
        *slash = '\0';
        TRY(namei(fs, buf, parent));
    }
    iget(fs, *parent, &dir);
    return dir.type == FS_DIR ? 0 : -ENOTDIR;
}
