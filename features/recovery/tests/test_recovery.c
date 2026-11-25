/*
 * test_recovery.c - after a crash, the filesystem comes back exactly as
 * it was at the last commit, including when a commit was cut off, the
 * primary superblock is damaged, or the newest root tree is.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkpoint.h"
#include "fault_disk.h"
#include "file_disk.h"
#include "fs.h"
#include "mem_disk.h"
#include "recovery.h"

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

#define BLOCKS 20000 /* big enough for the superblock copy at 64 MiB */

static int has(Fs *fs, const char *path, const char *want)
{
    char buf[256];
    int64_t n = fs_read(fs, path, 0, buf, sizeof buf - 1);
    if (!want)
        return n == -ENOENT;
    if (n < 0)
        return 0;
    buf[n] = '\0';
    return strcmp(buf, want) == 0;
}

static void put(Fs *fs, const char *path, const char *text)
{
    int r = fs_create(fs, path);
    CHECK(r == 0 || r == -EEXIST);
    CHECK(fs_truncate(fs, path, 0) == 0);
    CHECK(fs_write(fs, path, 0, text, strlen(text)) == (int64_t)strlen(text));
}

static Btrfs *crash_and_mount(Btrfs *b, Disk *d, int want_how)
{
    int how = -1;
    if (b)
        btrfs_crash(b); /* everything in memory is lost */
    CHECK(rec_mount(d, &b, &how) == 0);
    CHECK(how == want_how);
    CHECK(btrfs_check(d, 1) == 0);
    return b;
}

static void test_commit_and_crash(void)
{
    BtrfsMkfsOpts o = { 4096, NULL };
    Disk *d = mem_disk_create(8192);
    Btrfs *b;
    CHECK(rec_mount(d, &b, NULL) == -ENOENT); /* a blank disk */
    CHECK(rec_generation(d) == 0);
    CHECK(btrfs_mkfs(d, &o) == 0);
    uint64_t g0 = rec_generation(d);
    CHECK(g0 > 0);
    b = crash_and_mount(NULL, d, 0);
    Fs *fs = fs_open(b);
    CHECK(fs_mkdir(fs, "/home") == 0);
    put(fs, "/home/a.txt", "committed");
    CHECK(rec_commit(b) == 0);
    CHECK(rec_generation(d) == g0 + 1);

    put(fs, "/home/a.txt", "lost");
    put(fs, "/home/b.txt", "lost too");
    fs_close(fs);
    b = crash_and_mount(b, d, 0);
    fs = fs_open(b);
    CHECK(has(fs, "/home/a.txt", "committed"));
    CHECK(has(fs, "/home/b.txt", NULL));
    put(fs, "/home/b.txt", "second");
    fs_close(fs);
    CHECK(rec_commit(b) == 0);
    b = crash_and_mount(b, d, 0);
    fs = fs_open(b);
    CHECK(has(fs, "/home/b.txt", "second"));
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    disk_close(d);
    printf("ok   commit, crash, mount: exactly the committed state\n");
}

static void test_checkpoints_survive(void)
{
    BtrfsMkfsOpts o = { 4096, NULL };
    Disk *d = mem_disk_create(8192);
    Btrfs *b;
    char name[CKPT_NAME_MAX + 1];
    CHECK(btrfs_mkfs(d, &o) == 0 && btrfs_mount(d, NULL, &b) == 0 && ckpt_setup(b) == 0);
    Fs *fs = fs_open(b);
    put(fs, "/f", "version 1");
    CHECK(ckpt_create(b, "c1") == 0); /* a checkpoint commits */
    put(fs, "/f", "version 2");
    CHECK(ckpt_create(b, "c2") == 0);
    put(fs, "/f", "version 3");
    CHECK(ckpt_delete(b, "c1") == 0); /* not committed */
    fs_close(fs);
    b = crash_and_mount(b, d, 0);
    CHECK(ckpt_count(b) == 2);
    CHECK(ckpt_name_back(b, 1, name) == 0 && strcmp(name, "c2") == 0);
    fs = fs_open(b);
    CHECK(has(fs, "/f", "version 2")); /* c2's commit included the live data */
    fs_close(fs);
    CHECK(ckpt_rollback_back(b, 2) == 0);
    fs = fs_open(b);
    CHECK(has(fs, "/f", "version 1"));
    fs_close(fs);
    b = crash_and_mount(b, d, 0); /* the rollback committed too */
    fs = fs_open(b);
    CHECK(has(fs, "/f", "version 1"));
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    disk_close(d);
    printf("ok   checkpoints and rollbacks survive a crash\n");
}

static void test_damage(void)
{
    BtrfsMkfsOpts o = { 4096, NULL };
    Disk *d = mem_disk_create(BLOCKS);
    Btrfs *b;
    uint8_t junk[BLOCK_SIZE], sec[BLOCK_SIZE];
    memset(junk, 0x5a, sizeof junk);
    CHECK(btrfs_mkfs(d, &o) == 0 && btrfs_mount(d, NULL, &b) == 0);
    Fs *fs = fs_open(b);
    put(fs, "/f", "one");
    fs_close(fs);
    CHECK(rec_commit(b) == 0);

    /* A commit cut off half-way: power lost after a few of its writes. */
    Disk *fd = fault_disk_wrap(d);
    btrfs_crash(b);
    CHECK(btrfs_mount(fd, NULL, &b) == 0);
    fs = fs_open(b);
    put(fs, "/f", "two");
    fs_close(fs);
    fault_disk_cut_after(fd, 3);
    rec_commit(b);
    CHECK(fault_disk_dropped(fd) > 0);
    btrfs_crash(b);
    disk_close(fd);
    b = crash_and_mount(NULL, d, 0);
    fs = fs_open(b);
    CHECK(has(fs, "/f", "one"));
    fs_close(fs);

    /* The primary superblock damaged: super-recover from the mirror. */
    btrfs_crash(b);
    CHECK(disk_write(d, 16, junk) == 0);
    b = crash_and_mount(NULL, d, 2);
    fs = fs_open(b);
    CHECK(has(fs, "/f", "one"));
    put(fs, "/f", "three");
    fs_close(fs);
    CHECK(rec_commit(b) == 0);
    btrfs_crash(b);

    /* The newest root tree damaged: the backup roots. */
    BtrfsSuper sb;
    CHECK(disk_read(d, 16, &sb) == 0);
    CHECK(disk_read(d, (uint32_t)(sb.root / BLOCK_SIZE), sec) == 0);
    sec[300] ^= 0xff;
    CHECK(disk_write(d, (uint32_t)(sb.root / BLOCK_SIZE), sec) == 0);
    int how;
    CHECK(rec_mount(d, &b, &how) == 0 && how == 1);
    fs = fs_open(b);
    CHECK(has(fs, "/f", "one")); /* the commit before the damaged one */
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    CHECK(btrfs_check(d, 1) == 0);
    disk_close(d);
    printf("ok   a cut-off commit, a damaged superblock, a damaged root tree: each recovered\n");
}

static void test_file_disk(void)
{
    const char *path = "test_recovery.img";
    BtrfsMkfsOpts o = { 4096, NULL };
    Btrfs *b;
    remove(path);
    Disk *d = file_disk_open(path, 8192);
    CHECK(d && btrfs_mkfs(d, &o) == 0 && btrfs_mount(d, NULL, &b) == 0);
    Fs *fs = fs_open(b);
    put(fs, "/on-disk", "saved");
    fs_close(fs);
    CHECK(rec_commit(b) == 0);
    fs = fs_open(b);
    put(fs, "/on-disk", "never committed");
    fs_close(fs);
    btrfs_crash(b);
    disk_close(d); /* the program ends */

    d = file_disk_open(path, 8192);
    CHECK(d && rec_mount(d, &b, NULL) == 0);
    fs = fs_open(b);
    CHECK(has(fs, "/on-disk", "saved"));
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    CHECK(btrfs_check(d, 1) == 0);
    disk_close(d);
    remove(path);
    printf("ok   the state survives closing and reopening a file-backed disk\n");
}

/* Random work with commits and crashes, against a model. */
#define NF 8
static void test_random(void)
{
    BtrfsMkfsOpts o = { 4096, NULL };
    static char live[NF][32], committed[NF][32];
    Disk *d = mem_disk_create(16384);
    Btrfs *b;
    char p[16];
    int commits = 0, crashes = 0;
    CHECK(btrfs_mkfs(d, &o) == 0 && btrfs_mount(d, NULL, &b) == 0);
    Fs *fs = fs_open(b);
    srand(3);
    for (int step = 0; step < 5000; step++) {
        int op = rand() % 20, i = rand() % NF;
        snprintf(p, sizeof p, "/f%d", i);
        if (op < 12) {
            snprintf(live[i], sizeof live[i], "step %d", step);
            put(fs, p, live[i]);
        } else if (op < 15) {
            CHECK(fs_unlink(fs, p) == (live[i][0] ? 0 : -ENOENT));
            live[i][0] = 0;
        } else if (op < 18) {
            CHECK(rec_commit(b) == 0);
            memcpy(committed, live, sizeof live);
            commits++;
        } else {
            fs_close(fs);
            btrfs_crash(b);
            CHECK(rec_mount(d, &b, NULL) == 0);
            fs = fs_open(b);
            memcpy(live, committed, sizeof live);
            crashes++;
            if (crashes % 20 == 0)
                CHECK(btrfs_check(d, 1) == 0);
        }
        for (int j = 0; j < NF; j++) {
            snprintf(p, sizeof p, "/f%d", j);
            CHECK(has(fs, p, live[j][0] ? live[j] : NULL));
        }
    }
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    CHECK(btrfs_check(d, 1) == 0);
    disk_close(d);
    printf("ok   5000 random operations with %d commits and %d crashes against a model\n", commits, crashes);
}

int main(void)
{
    test_commit_and_crash();
    test_checkpoints_survive();
    test_damage();
    test_file_disk();
    test_random();
    printf("recovery: all tests passed\n");
    return 0;
}
