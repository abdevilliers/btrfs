/* test_util.h - shared by the tests: CHECK and a few helpers. */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"
#include "mem_disk.h"

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

/* A fresh filesystem on a RAM disk of `blocks` 4 KiB sectors, nodesize
 * 4096 (the smallest, so the trees grow tall quickly). */
static inline Disk *new_disk(uint32_t blocks)
{
    Disk *d = mem_disk_create(blocks);
    BtrfsMkfsOpts o = { 4096, "test" };
    CHECK(d != NULL);
    CHECK(btrfs_mkfs(d, &o) == 0);
    return d;
}

static inline Btrfs *mount_ok(Disk *d)
{
    Btrfs *b = NULL;
    CHECK(btrfs_mount(d, NULL, &b) == 0 && b != NULL);
    return b;
}

/* Commit, then btrfs check the disk: must be clean. */
static inline void commit_and_check(Btrfs *b)
{
    CHECK(btrfs_commit(b) == 0);
    int e = btrfs_check(btrfs_disk(b), 1);
    CHECK(e == 0);
}

static inline int file_is(Fs *fs, const char *path, const char *want)
{
    char buf[8192];
    int64_t n = fs_read(fs, path, 0, buf, sizeof buf - 1);
    if (!want)
        return n == -ENOENT;
    if (n < 0)
        return 0;
    buf[n] = '\0';
    return strcmp(buf, want) == 0 && (size_t)n == strlen(want);
}

static inline int put(Fs *fs, const char *path, const char *text)
{
    int r = fs_create(fs, path);
    if (r < 0 && r != -EEXIST)
        return r;
    if ((r = fs_truncate(fs, path, 0)) < 0)
        return r;
    int64_t w = fs_write(fs, path, 0, text, strlen(text));
    return w < 0 ? (int)w : 0;
}

#endif
