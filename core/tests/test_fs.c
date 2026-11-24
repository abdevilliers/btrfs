/*
 * test_fs.c - the filesystem: files, directories, the inode free list,
 * hard links, truncate, a full disk, and random operations checked
 * against a simple model with fsck (fs_check) after every step.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"
#include "mem_disk.h"
#include "volume.h"

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

static Disk *disk;
static Volume *vol;
static Fs *fs;

static void setup(uint32_t blocks)
{
    disk = mem_disk_create(blocks);
    vol = vol_create(disk);
    CHECK(fs_open(vol) == NULL);        /* nothing formatted yet */
    CHECK(fs_format(vol) == 0);
    fs = fs_open(vol);
    CHECK(fs != NULL);
    CHECK(fs_check(fs) == 0);
}

static void teardown(void)
{
    CHECK(fs_check(fs) == 0);
    CHECK(vol_check(vol) == 0);
    fs_close(fs);
    vol_destroy(vol);
    disk_close(disk);
}

static int file_is(const char *path, const char *want)
{
    char buf[FS_MAX_FILE + 1];
    int n = fs_read(fs, path, 0, buf, FS_MAX_FILE);
    if (n < 0)
        return 0;
    buf[n] = '\0';
    return strcmp(buf, want) == 0 && (size_t)n == strlen(want);
}

static int put(const char *path, const char *text)
{
    int r = fs_create(fs, path);
    if (r < 0 && r != -EEXIST)
        return r;
    r = fs_truncate(fs, path, 0);
    if (r < 0)
        return r;
    r = fs_write(fs, path, 0, text, (uint32_t)strlen(text));
    return r < 0 ? r : 0;
}

/* ---------------------------------------------------------------- tests */

static int count;
static void count_entry(void *arg, const char *name, const FsStat *st)
{
    (void)arg;
    (void)name;
    (void)st;
    count++;
}

static void test_basics(void)
{
    FsStat st;

    setup(512);
    CHECK(fs_stat(fs, "/", &st) == 0 && st.type == FS_DIR && st.ino == FS_ROOT_INO);
    CHECK(st.nlink == 2);

    CHECK(fs_mkdir(fs, "/docs") == 0);
    CHECK(fs_create(fs, "/docs/notes.txt") == 0);
    CHECK(fs_write(fs, "/docs/notes.txt", 0, "hello", 5) == 5);
    CHECK(file_is("/docs/notes.txt", "hello"));
    CHECK(fs_write(fs, "/docs/notes.txt", 5, ", world", 7) == 7);   /* append */
    CHECK(file_is("/docs/notes.txt", "hello, world"));
    CHECK(fs_write(fs, "/docs/notes.txt", 0, "J", 1) == 1);         /* overwrite */
    CHECK(file_is("/docs/notes.txt", "Jello, world"));

    CHECK(fs_stat(fs, "/docs/notes.txt", &st) == 0);
    CHECK(st.type == FS_FILE && st.size == 12 && st.nlink == 1 && st.blocks == 1);
    CHECK(fs_stat(fs, "/docs", &st) == 0 && st.nlink == 2);
    CHECK(fs_stat(fs, "/", &st) == 0 && st.nlink == 3);              /* /docs/.. */
    CHECK(fs_stat(fs, "//docs//notes.txt", &st) == 0);              /* extra slashes */

    count = 0;
    CHECK(fs_readdir(fs, "/docs", count_entry, NULL) == 0);
    CHECK(count == 3);                                               /* . .. notes.txt */

    CHECK(fs_create(fs, "/docs/notes.txt") == -EEXIST);
    CHECK(fs_create(fs, "/missing/x") == -ENOENT);
    CHECK(fs_create(fs, "/docs/notes.txt/x") == -ENOTDIR);
    CHECK(fs_create(fs, "relative") == -EINVAL);
    CHECK(fs_create(fs, "/") == -EINVAL);
    CHECK(fs_create(fs, "/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == -ENAMETOOLONG);
    CHECK(fs_read(fs, "/docs", 0, (char[8]){ 0 }, 8) == -EISDIR);
    CHECK(fs_write(fs, "/docs", 0, "x", 1) == -EISDIR);
    CHECK(fs_unlink(fs, "/docs") == -EISDIR);
    CHECK(fs_rmdir(fs, "/docs") == -ENOTEMPTY);
    CHECK(fs_rmdir(fs, "/docs/notes.txt") == -ENOTDIR);
    CHECK(fs_check(fs) == 0);

    CHECK(fs_unlink(fs, "/docs/notes.txt") == 0);
    CHECK(fs_stat(fs, "/docs/notes.txt", &st) == -ENOENT);
    CHECK(fs_rmdir(fs, "/docs") == 0);
    CHECK(fs_stat(fs, "/", &st) == 0 && st.nlink == 2);
    teardown();
    printf("ok   files and directories: create, write, read, stat, readdir, unlink, rmdir\n");
}

static void test_inode_free_list(void)
{
    FsStat st;
    FsInfo info;
    char path[32];

    setup(1024);
    CHECK(fs_info(fs, &info) == 0 && info.inodes_free == FS_INODES - 2);

    CHECK(fs_create(fs, "/a") == 0);              /* the free list starts 2, 3, 4 ... */
    CHECK(fs_stat(fs, "/a", &st) == 0 && st.ino == 2);
    CHECK(fs_create(fs, "/b") == 0);
    CHECK(fs_stat(fs, "/b", &st) == 0 && st.ino == 3);
    CHECK(fs_unlink(fs, "/a") == 0);              /* inode 2 goes back on the front */
    CHECK(fs_create(fs, "/c") == 0);
    CHECK(fs_stat(fs, "/c", &st) == 0 && st.ino == 2); /* ... so it is reused first */
    CHECK(fs_unlink(fs, "/b") == 0);
    CHECK(fs_unlink(fs, "/c") == 0);

    /* Use up every inode. A directory holds 46 names, so use two. */
    int made = 0, r = 0;
    CHECK(fs_mkdir(fs, "/d1") == 0);
    CHECK(fs_mkdir(fs, "/d2") == 0);
    made += 2;
    for (int i = 0; r == 0; i++) {
        snprintf(path, sizeof path, "/d%d/f%d", 1 + i % 2, i);
        r = fs_create(fs, path);
        if (r == 0)
            made++;
    }
    CHECK(r == -ENOSPC);                          /* the free list is empty */
    CHECK(made == FS_INODES - 2);
    CHECK(fs_info(fs, &info) == 0 && info.inodes_free == 0);
    CHECK(fs_mkdir(fs, "/e") == -ENOSPC);
    CHECK(fs_check(fs) == 0);

    CHECK(fs_unlink(fs, "/d1/f8") == 0);          /* free one ... */
    CHECK(fs_create(fs, "/new") == 0);            /* ... and it is the one reused */
    CHECK(fs_info(fs, &info) == 0 && info.inodes_free == 0);
    teardown();
    printf("ok   inodes: lowest free first, a freed inode is reused first, run out\n");
}

/* Bach's ialloc and ifree: the superblock's free inode list and the
 * remembered inode. */
static int ino_of(const char *path)
{
    FsStat st;
    return fs_stat(fs, path, &st) == 0 ? (int)st.ino : -1;
}

static void test_ialloc_ifree(void)
{
    FsInfo in;
    char path[16];

    setup(1024);
    /* mkfs ran ialloc once for "/": the scan found inodes 1..8, handed out
     * 1, and remembered 8. The next one out is the top of the list: 2. */
    CHECK(fs_info(fs, &in) == 0);
    CHECK(in.ninode == 7 && in.inode_list[6] == 2 && in.remembered_inode == 8);

    for (int i = 2; i <= 8; i++) {                  /* use up the list: 2 .. 8 */
        snprintf(path, sizeof path, "/f%d", i);
        CHECK(fs_create(fs, path) == 0 && ino_of(path) == i);
    }
    CHECK(fs_info(fs, &in) == 0 && in.ninode == 0);

    CHECK(fs_create(fs, "/f9") == 0);               /* list empty: scan from 8 */
    CHECK(ino_of("/f9") == 9);
    CHECK(fs_info(fs, &in) == 0 && in.ninode == 7 && in.remembered_inode == 16);

    CHECK(fs_unlink(fs, "/f3") == 0);               /* list not full: 3 goes in */
    CHECK(fs_info(fs, &in) == 0 && in.ninode == 8 && in.inode_list[7] == 3);
    CHECK(fs_unlink(fs, "/f4") == 0);               /* list full: remember 4 instead */
    CHECK(fs_info(fs, &in) == 0 && in.ninode == 8 && in.remembered_inode == 4);
    CHECK(fs_check(fs) == 0);

    CHECK(fs_create(fs, "/a") == 0 && ino_of("/a") == 3); /* from the list */
    for (int i = 10; i <= 16; i++) {                      /* the rest: 10 .. 16 */
        snprintf(path, sizeof path, "/g%d", i);
        CHECK(fs_create(fs, path) == 0 && ino_of(path) == i);
    }
    CHECK(fs_create(fs, "/b") == 0);                /* empty again: scan from 4 ... */
    CHECK(ino_of("/b") == 4);                       /* ... and 4 is found first */
    CHECK(fs_info(fs, &in) == 0 && in.remembered_inode == 23); /* 4, 17 .. 23 */
    teardown();
    printf("ok   ialloc / ifree: free inode list, scan from the remembered inode\n");
}

/* Bach's alloc and free: the chained free block list. */
static void test_alloc_free(void)
{
    FsInfo in, start;
    FsStat st;
    char data[FS_MAX_FILE];
    uint32_t bno[FS_DIRECT];

    memset(data, 'x', sizeof data);
    setup(1024);
    CHECK(fs_info(fs, &start) == 0);
    CHECK(start.blocks_free == start.blocks_total - 1);  /* "/" uses one block */
    uint32_t k = start.nfree;                            /* entries in the list now */
    uint32_t next = start.free_list[k - 1], link = start.free_list[0];
    CHECK(k >= 2 && link != 0);

    /* alloc hands out the top of the list first. */
    CHECK(fs_create(fs, "/one") == 0);
    CHECK(fs_write(fs, "/one", 0, data, 1) == 1);
    CHECK(fs_bmap(fs, "/one", 0, 0, &bno[0]) == 0 && bno[0] == next);
    CHECK(fs_info(fs, &in) == 0 && in.nfree == k - 1);

    /* Take every block left in the list: the last one taken is free[0],
     * the block holding the next part of the list, which is copied into
     * the superblock. */
    CHECK(fs_create(fs, "/many") == 0);
    CHECK(fs_write(fs, "/many", 0, data, (k - 1) * BLOCK_SIZE) == (int)((k - 1) * BLOCK_SIZE));
    CHECK(fs_bmap(fs, "/many", k - 2, 0, &bno[1]) == 0 && bno[1] == link);
    CHECK(fs_info(fs, &in) == 0 && in.nfree == FS_NICFREE); /* refilled from the link block */
    CHECK(in.blocks_free == start.blocks_free - k);
    CHECK(fs_check(fs) == 0);

    /* free: with the list full, the first block freed stores the list and
     * becomes free[0] of a new list. */
    CHECK(fs_stat(fs, "/many", &st) == 0);
    uint32_t first;
    CHECK(fs_bmap(fs, "/many", 0, 0, &first) == 0);
    CHECK(fs_unlink(fs, "/many") == 0);
    CHECK(fs_info(fs, &in) == 0 && in.free_list[0] == first && in.nfree == k - 1);
    CHECK(fs_unlink(fs, "/one") == 0);
    CHECK(fs_info(fs, &in) == 0 && in.blocks_free == start.blocks_free);
    CHECK(in.free_list[in.nfree - 1] == next);           /* last freed = next out */
    teardown();
    printf("ok   alloc / free: chained free block list, refill and spill\n");
}

static void test_links(void)
{
    FsStat st;
    FsInfo before, after;

    setup(512);
    CHECK(put("/orig", "shared text") == 0);
    CHECK(fs_mkdir(fs, "/dir") == 0);
    CHECK(fs_info(fs, &before) == 0);
    CHECK(fs_link(fs, "/orig", "/dir/alias") == 0);  /* a second name, same inode */
    CHECK(fs_stat(fs, "/orig", &st) == 0 && st.nlink == 2);
    CHECK(file_is("/dir/alias", "shared text"));
    CHECK(fs_write(fs, "/dir/alias", 0, "S", 1) == 1); /* both names see the change */
    CHECK(file_is("/orig", "Shared text"));
    CHECK(fs_link(fs, "/orig", "/dir/alias") == -EEXIST);
    CHECK(fs_link(fs, "/dir", "/dir2") == -EPERM);     /* no links to directories */
    CHECK(fs_check(fs) == 0);

    CHECK(fs_unlink(fs, "/orig") == 0);              /* the data stays: one link left */
    CHECK(file_is("/dir/alias", "Shared text"));
    CHECK(fs_stat(fs, "/dir/alias", &st) == 0 && st.nlink == 1);
    CHECK(fs_unlink(fs, "/dir/alias") == 0);         /* last link: inode and page freed */
    CHECK(fs_info(fs, &after) == 0);
    CHECK(after.inodes_free == before.inodes_free + 1);
    CHECK(after.blocks_free == before.blocks_free + 1);
    teardown();
    printf("ok   hard links: nlink counts, data freed with the last link\n");
}

static void test_sizes(void)
{
    const int MAX = (int)FS_MAX_FILE;
    char big[FS_MAX_FILE], back[FS_MAX_FILE];
    FsStat st;

    setup(512);
    for (int i = 0; i < MAX; i++)
        big[i] = (char)('a' + i % 26);
    CHECK(fs_create(fs, "/big") == 0);
    CHECK(fs_write(fs, "/big", 0, big, FS_MAX_FILE) == MAX);      /* all 12 pages */
    CHECK(fs_read(fs, "/big", 0, back, FS_MAX_FILE) == MAX);
    CHECK(memcmp(big, back, FS_MAX_FILE) == 0);
    CHECK(fs_write(fs, "/big", FS_MAX_FILE, "x", 1) == -EFBIG);
    CHECK(fs_read(fs, "/big", FS_MAX_FILE, back, 10) == 0);          /* end of file */

    CHECK(fs_truncate(fs, "/big", 200) == 0);    /* frees pages 2..11 */
    CHECK(fs_stat(fs, "/big", &st) == 0 && st.size == 200 && st.blocks == 2);
    CHECK(fs_truncate(fs, "/big", 300) == 0);    /* grow: the new part reads as zeros */
    CHECK(fs_read(fs, "/big", 0, back, 300) == 300);
    CHECK(memcmp(back, big, 200) == 0);
    for (int i = 200; i < 300; i++)
        CHECK(back[i] == 0);

    CHECK(fs_create(fs, "/sparse") == 0);        /* a gap before the data */
    CHECK(fs_write(fs, "/sparse", 1000, "end", 3) == 3);
    CHECK(fs_stat(fs, "/sparse", &st) == 0 && st.size == 1003 && st.blocks == 1);
    CHECK(fs_read(fs, "/sparse", 0, back, 1003) == 1003);
    CHECK(back[0] == 0 && back[999] == 0 && memcmp(back + 1000, "end", 3) == 0);
    teardown();
    printf("ok   file sizes: 12 direct blocks, truncate, gaps read as zeros\n");
}

static void test_full_disk_is_atomic(void)
{
    char data[FS_MAX_FILE];
    char path[32];
    FsInfo info;
    int r = 0, files = 0;

    memset(data, 'z', sizeof data);
    setup(300); /* a small disk */
    CHECK(fs_mkdir(fs, "/d") == 0);
    for (int i = 0; r >= 0; i++) {
        snprintf(path, sizeof path, "/d/f%d", i);
        r = fs_create(fs, path);
        if (r == 0) {
            r = fs_write(fs, path, 0, data, sizeof data);
            if (r >= 0)
                files++;
            else
                CHECK(fs_stat(fs, path, &(FsStat){ 0 }) == 0); /* create stayed, write undone */
        }
        CHECK(fs_check(fs) == 0);
        CHECK(vol_check(vol) == 0);
    }
    CHECK(r == -ENOSPC);
    CHECK(files > 0);
    CHECK(fs_info(fs, &info) == 0);
    teardown();
    printf("ok   a full disk fails cleanly: each operation is all or nothing (%d files fit)\n", files);
}

/* Random operations on a few names, compared with a model in memory. */
#define NAMES 8
static const char *names[NAMES] = { "/a", "/b", "/c", "/d/a", "/d/b", "/d/e/x", "/f", "/d/e/y" };

typedef struct {
    int exists;
    uint32_t len;
    char data[FS_MAX_FILE];
} ModelFile;

static void test_random(void)
{
    static ModelFile model[NAMES];
    char buf[FS_MAX_FILE];
    int ops = 0;

    setup(2048);
    CHECK(fs_mkdir(fs, "/d") == 0);
    CHECK(fs_mkdir(fs, "/d/e") == 0);
    memset(model, 0, sizeof model);
    srand(5);
    for (int step = 0; step < 4000; step++) {
        int k = rand() % NAMES, op = rand() % 6;
        ModelFile *m = &model[k];
        if (op == 0) {                                     /* create */
            int r = fs_create(fs, names[k]);
            CHECK(r == (m->exists ? -EEXIST : 0));
            if (!m->exists) {
                m->exists = 1;
                m->len = 0;
            }
        } else if (op <= 2) {                              /* write at an offset */
            uint32_t off = (uint32_t)(rand() % 1400), n = (uint32_t)(rand() % 130 + 1);
            for (uint32_t i = 0; i < n; i++)
                buf[i] = (char)('A' + (step + (int)i) % 26);
            int r = fs_write(fs, names[k], off, buf, n);
            if (!m->exists) {
                CHECK(r == -ENOENT);
            } else {
                CHECK(r == (int)n);
                if (off > m->len)
                    memset(m->data + m->len, 0, off - m->len);
                memcpy(m->data + off, buf, n);
                if (off + n > m->len)
                    m->len = off + n;
            }
        } else if (op == 3) {                              /* truncate */
            uint32_t size = (uint32_t)(rand() % 1500);
            int r = fs_truncate(fs, names[k], size);
            CHECK(r == (m->exists ? 0 : -ENOENT));
            if (m->exists) {
                if (size > m->len)
                    memset(m->data + m->len, 0, size - m->len);
                m->len = size;
            }
        } else if (op == 4) {                              /* unlink */
            CHECK(fs_unlink(fs, names[k]) == (m->exists ? 0 : -ENOENT));
            m->exists = 0;
        } else {                                           /* read and compare */
            int r = fs_read(fs, names[k], 0, buf, FS_MAX_FILE);
            if (!m->exists)
                CHECK(r == -ENOENT);
            else
                CHECK(r == (int)m->len && memcmp(buf, m->data, m->len) == 0);
        }
        ops++;
        CHECK(fs_check(fs) == 0);  /* fsck after every step */
        CHECK(vol_check(vol) == 0);
    }
    teardown();
    printf("ok   %d random creates, writes, truncates, unlinks and reads, fsck after each\n", ops);
}

int main(void)
{
    test_basics();
    test_inode_free_list();
    test_ialloc_ifree();
    test_alloc_free();
    test_links();
    test_sizes();
    test_full_disk_is_atomic();
    test_random();
    printf("fs: all tests passed\n");
    return 0;
}
