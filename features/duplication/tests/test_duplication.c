/*
 * test_duplication.c - a reflink copy shares the data extents, copies no
 * data, and both files stay independent afterwards.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "duplication.h"
#include "fs.h"
#include "mem_disk.h"

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

#define SZ (5 * BLOCK_SIZE + 100) /* 6 sectors, the last one partial */

static Disk *disk;
static Btrfs *b;

static Fs *setup(void)
{
    BtrfsMkfsOpts o = { 4096, NULL };
    disk = mem_disk_create(16384);
    CHECK(btrfs_mkfs(disk, &o) == 0);
    CHECK(btrfs_mount(disk, NULL, &b) == 0);
    return fs_open(b);
}

static void teardown(Fs *fs)
{
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    CHECK(btrfs_check(disk, 1) == 0);
    disk_close(disk);
}

static int same_data(Fs *fs, const char *path, const char *want, uint64_t len)
{
    static char buf[SZ + 10];
    return fs_read(fs, path, 0, buf, sizeof buf) == (int64_t)len && memcmp(buf, want, len) == 0;
}

static void test_copy_file(void)
{
    static char text[SZ];
    Fs *fs = setup();
    FsSpace s0, s1;
    FsStat a, c;
    BtrfsStats st0, st1;

    for (int i = 0; i < (int)SZ; i++)
        text[i] = (char)('a' + i % 26);
    CHECK(fs_create(fs, "/big.txt") == 0);
    CHECK(fs_write(fs, "/big.txt", 0, text, SZ) == SZ);
    CHECK(btrfs_commit(b) == 0);
    fs_space(b, &s0);
    btrfs_stats(b, &st0);

    CHECK(dup_copy_file(fs, "/big.txt", "/copy.txt") == 0);
    btrfs_stats(b, &st1);
    CHECK(st1.data_sectors_written == st0.data_sectors_written); /* no data written */
    CHECK(btrfs_commit(b) == 0);
    fs_space(b, &s1);
    CHECK(s1.data_used == s0.data_used); /* no data extent added */
    CHECK(fs_stat(fs, "/big.txt", &a) == 0 && fs_stat(fs, "/copy.txt", &c) == 0);
    CHECK(a.ino != c.ino && c.size == SZ && c.nbytes == a.nbytes);
    CHECK(same_data(fs, "/copy.txt", text, SZ));
    for (uint64_t off = 0; off < SZ; off += BLOCK_SIZE)
        CHECK(dup_is_shared(fs, "/big.txt", off, "/copy.txt", off));
    CHECK(btrfs_check(disk, 1) == 0);

    /* Writing the copy changes only the sector written. */
    CHECK(fs_write(fs, "/copy.txt", BLOCK_SIZE + 3, "XYZ", 3) == 3);
    CHECK(same_data(fs, "/big.txt", text, SZ));
    CHECK(!dup_is_shared(fs, "/big.txt", BLOCK_SIZE, "/copy.txt", BLOCK_SIZE));
    CHECK(dup_is_shared(fs, "/big.txt", 0, "/copy.txt", 0));
    CHECK(dup_is_shared(fs, "/big.txt", 3 * BLOCK_SIZE, "/copy.txt", 3 * BLOCK_SIZE));

    /* The copy keeps its data when the original goes. */
    CHECK(fs_unlink(fs, "/big.txt") == 0);
    text[BLOCK_SIZE + 3] = 'X';
    text[BLOCK_SIZE + 4] = 'Y';
    text[BLOCK_SIZE + 5] = 'Z';
    CHECK(same_data(fs, "/copy.txt", text, SZ));
    CHECK(btrfs_commit(b) == 0);
    CHECK(btrfs_check(disk, 1) == 0);

    CHECK(dup_copy_file(fs, "/missing", "/x") == -ENOENT);
    CHECK(fs_mkdir(fs, "/dir") == 0);
    CHECK(dup_copy_file(fs, "/dir", "/x") == -EISDIR);
    CHECK(dup_copy_file(fs, "/copy.txt", "/dir") == -EEXIST);
    teardown(fs);
    printf("ok   copy a file: new inode, shared extents, no data written, split on write\n");
}

static void test_ranges_and_subvolumes(void)
{
    static char a[8 * BLOCK_SIZE], back[8 * BLOCK_SIZE];
    Fs *fs = setup();

    memset(a, 'A', sizeof a);
    memset(a + 4 * BLOCK_SIZE, 'B', 4 * BLOCK_SIZE);
    CHECK(fs_create(fs, "/a") == 0 && fs_write(fs, "/a", 0, a, sizeof a) == (int64_t)sizeof a);
    CHECK(fs_create(fs, "/b") == 0);
    CHECK(fs_write(fs, "/b", 0, "................", 16) == 16);
    /* The B half of /a into the start of /b. */
    CHECK(fs_truncate(fs, "/b", 4 * BLOCK_SIZE) == 0);
    CHECK(dup_copy_range(fs, "/a", 4 * BLOCK_SIZE, "/b", 0, 2 * BLOCK_SIZE) == 0);
    CHECK(fs_read(fs, "/b", 0, back, 2 * BLOCK_SIZE) == 2 * BLOCK_SIZE && back[0] == 'B' &&
          back[2 * BLOCK_SIZE - 1] == 'B');
    CHECK(dup_is_shared(fs, "/a", 4 * BLOCK_SIZE, "/b", 0));
    CHECK(dup_copy_range(fs, "/a", 1, "/b", 0, BLOCK_SIZE) == -EINVAL); /* not aligned */
    CHECK(dup_copy_range(fs, "/a", 0, "/a", BLOCK_SIZE, 2 * BLOCK_SIZE) == -EINVAL); /* overlap */

    /* Across subvolumes (cp --reflink between subvolumes). */
    CHECK(fs_subvol_create(fs, "/vol") == 0);
    CHECK(dup_copy_file(fs, "/a", "/vol/a") == 0);
    CHECK(dup_is_shared(fs, "/a", 0, "/vol/a", 0));
    CHECK(fs_read(fs, "/vol/a", 0, back, sizeof back) == (int64_t)sizeof a && memcmp(back, a, sizeof a) == 0);

    /* A small (inline) file is copied, not shared: its data is in the tree. */
    CHECK(fs_create(fs, "/tiny") == 0 && fs_write(fs, "/tiny", 0, "hi", 2) == 2);
    CHECK(dup_copy_file(fs, "/tiny", "/tiny2") == 0);
    CHECK(fs_read(fs, "/tiny2", 0, back, 10) == 2 && memcmp(back, "hi", 2) == 0);
    teardown(fs);
    printf("ok   ranges, across subvolumes, inline files\n");
}

/* Random writes and copies on a few files, against a model. */
#define NF 6
#define MAXF (12 * BLOCK_SIZE)
static void test_random(void)
{
    static unsigned char model[NF][MAXF], buf[MAXF];
    static uint64_t len[NF];
    Fs *fs = setup();
    char p[16], q[16];

    for (int i = 0; i < NF; i++) {
        snprintf(p, sizeof p, "/f%d", i);
        CHECK(fs_create(fs, p) == 0);
    }
    srand(11);
    for (int step = 0; step < 3000; step++) {
        int i = rand() % NF, j = rand() % NF, op = rand() % 4;
        snprintf(p, sizeof p, "/f%d", i);
        snprintf(q, sizeof q, "/f%d", j);
        if (op == 0) {
            uint64_t off = (uint64_t)rand() % (MAXF - 9000), n = (uint64_t)(rand() % 9000 + 1);
            for (uint64_t k = 0; k < n; k++)
                buf[k] = (unsigned char)rand();
            CHECK(fs_write(fs, p, off, buf, n) == (int64_t)n);
            if (off > len[i])
                memset(model[i] + len[i], 0, off - len[i]);
            memcpy(model[i] + off, buf, n);
            if (off + n > len[i])
                len[i] = off + n;
        } else if (op == 1 && i != j) {
            CHECK(fs_truncate(fs, q, 0) == 0);
            CHECK(fs_clone_range(fs, p, 0, q, 0, 0) == 0);
            memcpy(model[j], model[i], len[i]);
            len[j] = len[i];
        } else {
            CHECK(fs_read(fs, p, 0, buf, MAXF) == (int64_t)len[i] && memcmp(buf, model[i], len[i]) == 0);
        }
        if (step % 300 == 0) {
            CHECK(btrfs_commit(b) == 0);
            CHECK(btrfs_check(disk, 1) == 0);
        }
    }
    teardown(fs);
    printf("ok   3000 random writes and reflink copies against a model\n");
}

int main(void)
{
    test_copy_file();
    test_ranges_and_subvolumes();
    test_random();
    printf("duplication: all tests passed\n");
    return 0;
}
