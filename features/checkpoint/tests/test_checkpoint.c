/*
 * test_checkpoint.c - checkpoints by name, by position, at fixed times;
 * rollback; deleting; and random work against a model with btrfs check.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkpoint.h"
#include "fs.h"
#include "mem_disk.h"

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

static Disk *disk;
static Btrfs *b;

static Fs *setup(uint32_t blocks)
{
    BtrfsMkfsOpts o = { 4096, NULL };
    disk = mem_disk_create(blocks);
    CHECK(btrfs_mkfs(disk, &o) == 0);
    CHECK(btrfs_mount(disk, NULL, &b) == 0);
    CHECK(ckpt_setup(b) == 0);
    CHECK(ckpt_setup(b) == 0); /* twice is fine */
    Fs *fs = fs_open(b);
    CHECK(fs && fs_subvol_id(fs) != BTRFS_FS_TREE_OBJECTID); /* the live subvolume "@" */
    return fs;
}

static void teardown(Fs *fs)
{
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    CHECK(btrfs_check(disk, 1) == 0);
    disk_close(disk);
}

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

static void test_basics(void)
{
    Fs *fs = setup(8192);
    FsSpace s0, s1;

    CHECK(fs_mkdir(fs, "/docs") == 0);
    put(fs, "/docs/report.txt", "draft");
    put(fs, "/docs/notes.txt", "meeting at 10");
    CHECK(btrfs_commit(b) == 0);
    fs_space(b, &s0);

    CHECK(ckpt_create(b, "monday") == 0);
    fs_space(b, &s1);
    CHECK(s1.bytes_used <= s0.bytes_used + 2 * 4096); /* one copied root block */
    CHECK(ckpt_create(b, "monday") == -EEXIST);
    CHECK(ckpt_create(b, "bad/name") == -EINVAL);
    CHECK(ckpt_count(b) == 1);

    put(fs, "/docs/report.txt", "FINAL");
    CHECK(fs_unlink(fs, "/docs/notes.txt") == 0);
    put(fs, "/docs/todo.txt", "buy milk");

    Fs *old = ckpt_open_fs(b, "monday");
    CHECK(old != NULL);
    CHECK(has(old, "/docs/report.txt", "draft"));
    CHECK(has(old, "/docs/notes.txt", "meeting at 10"));
    CHECK(has(old, "/docs/todo.txt", NULL));
    CHECK(fs_create(old, "/x") == -EROFS); /* a read-only snapshot */
    fs_close(old);
    CHECK(ckpt_open_fs(b, "nope") == NULL);

    CHECK(ckpt_rollback(b, "monday") == 0);
    CHECK(fs_read(fs, "/docs/report.txt", 0, (char[8]){ 0 }, 8) == -ESTALE); /* the old live subvolume is gone */
    fs_close(fs);
    fs = fs_open(b);
    CHECK(has(fs, "/docs/report.txt", "draft"));
    CHECK(has(fs, "/docs/notes.txt", "meeting at 10"));
    CHECK(has(fs, "/docs/todo.txt", NULL));
    put(fs, "/docs/report.txt", "draft 2"); /* the rolled-back filesystem is writable */
    old = ckpt_open_fs(b, "monday");
    CHECK(has(old, "/docs/report.txt", "draft")); /* the checkpoint did not change */
    fs_close(old);
    CHECK(btrfs_check(disk, 1) == 0);

    CHECK(ckpt_delete(b, "monday") == 0);
    CHECK(ckpt_delete(b, "monday") == -ENOENT);
    CHECK(ckpt_count(b) == 0);
    teardown(fs);
    printf("ok   create (one block), read an old file, rollback, delete\n");
}

static void test_go_back(void)
{
    Fs *fs = setup(8192);
    char name[CKPT_NAME_MAX + 1], text[32];

    CHECK(ckpt_rollback_back(b, 1) == -ENOENT);
    for (int i = 1; i <= 5; i++) {
        snprintf(text, sizeof text, "version %d", i);
        snprintf(name, sizeof name, "c%d", i);
        put(fs, "/f", text);
        CHECK(ckpt_create(b, name) == 0);
    }
    put(fs, "/f", "not saved");
    CHECK(ckpt_count(b) == 5);
    CHECK(ckpt_name_back(b, 1, name) == 0 && strcmp(name, "c5") == 0);
    CHECK(ckpt_name_back(b, 5, name) == 0 && strcmp(name, "c1") == 0);
    CHECK(ckpt_name_back(b, 6, name) == -ENOENT);

    fs_close(fs);
    CHECK(ckpt_rollback_back(b, 1) == 0);
    fs = fs_open(b);
    CHECK(has(fs, "/f", "version 5"));
    fs_close(fs);
    CHECK(ckpt_rollback_back(b, 4) == 0);
    fs = fs_open(b);
    CHECK(has(fs, "/f", "version 2"));
    fs_close(fs);
    CHECK(ckpt_rollback_back(b, 2) == 0); /* nothing was deleted: forward again */
    fs = fs_open(b);
    CHECK(has(fs, "/f", "version 4"));
    CHECK(ckpt_rollback_back(b, 0) == -ENOENT);
    CHECK(ckpt_rollback_back(b, 6) == -ENOENT);
    CHECK(has(fs, "/f", "version 4")); /* a failed rollback changes nothing */
    CHECK(ckpt_delete(b, "c4") == 0);
    CHECK(ckpt_name_back(b, 2, name) == 0 && strcmp(name, "c3") == 0);
    CHECK(ckpt_create(b, "late") == 0);
    CHECK(ckpt_name_back(b, 1, name) == 0 && strcmp(name, "late") == 0);
    teardown(fs);
    printf("ok   go back x checkpoints\n");
}

static void test_timer(void)
{
    Fs *fs = setup(8192);
    char name[CKPT_NAME_MAX + 1], text[32];

    CHECK(ckpt_timer_create(b, 0, 3, 0) == NULL);
    CkptTimer *t = ckpt_timer_create(b, 10, 3, 4);
    CHECK(t && ckpt_timer_next(t) == 10);
    CHECK(ckpt_create(b, "by-hand") == 0);
    int taken = 0;
    for (uint64_t now = 5; now <= 45; now++) {
        snprintf(text, sizeof text, "written at %d", (int)now);
        put(fs, "/f", text);
        int r = ckpt_timer_tick(t, now);
        CHECK(r == (now % 10 == 0));
        taken += r;
    }
    CHECK(taken == 4 && ckpt_timer_next(t) == 50);
    CHECK(ckpt_count(b) == 4); /* auto-20, -30, -40 and by-hand */
    Fs *old = ckpt_open_fs(b, "auto-20");
    CHECK(old && has(old, "/f", "written at 20"));
    fs_close(old);
    CHECK(ckpt_open_fs(b, "auto-10") == NULL);
    CHECK(ckpt_name_back(b, 1, name) == 0 && strcmp(name, "auto-40") == 0);
    fs_close(fs);
    CHECK(ckpt_rollback_back(b, 2) == 0);
    fs = fs_open(b);
    CHECK(has(fs, "/f", "written at 30"));
    CHECK(ckpt_timer_tick(t, 67) == 1); /* 50 and 60 were missed: one covers them */
    CHECK(ckpt_name_back(b, 1, name) == 0 && strcmp(name, "auto-60") == 0);
    ckpt_timer_destroy(t);
    teardown(fs);
    printf("ok   checkpoints at fixed times, keeping the newest 3\n");
}

/* Random work: writes, checkpoints, rollbacks, deletes, against a model. */
#define NF 6
#define NC 4
typedef char Files[NF][32];
static void test_random(void)
{
    Fs *fs = setup(16384);
    static Files live, snap[NC];
    int has_c[NC] = { 0 }, rollbacks = 0;
    char p[16], name[16];

    srand(7);
    for (int step = 0; step < 1500; step++) {
        int op = rand() % 12, i = rand() % NF, k = rand() % NC;
        snprintf(p, sizeof p, "/f%d", i);
        snprintf(name, sizeof name, "c%d", k);
        if (op < 6) {
            snprintf(live[i], sizeof live[i], "step %d", step);
            put(fs, p, live[i]);
        } else if (op < 7) {
            CHECK(fs_unlink(fs, p) == (live[i][0] ? 0 : -ENOENT));
            live[i][0] = 0;
        } else if (op < 8 && !has_c[k]) {
            CHECK(ckpt_create(b, name) == 0);
            memcpy(snap[k], live, sizeof live);
            has_c[k] = 1;
        } else if (op < 9 && has_c[k]) {
            fs_close(fs);
            CHECK(ckpt_rollback(b, name) == 0);
            fs = fs_open(b);
            memcpy(live, snap[k], sizeof live);
            rollbacks++;
        } else if (op < 10 && has_c[k]) {
            CHECK(ckpt_delete(b, name) == 0);
            has_c[k] = 0;
        } else {
            for (int j = 0; j < NF; j++) {
                snprintf(p, sizeof p, "/f%d", j);
                CHECK(has(fs, p, live[j][0] ? live[j] : NULL));
                if (has_c[k]) {
                    Fs *old = ckpt_open_fs(b, name);
                    CHECK(old && has(old, p, snap[k][j][0] ? snap[k][j] : NULL));
                    fs_close(old);
                }
            }
        }
        if (step % 100 == 0) {
            CHECK(btrfs_commit(b) == 0);
            CHECK(btrfs_check(disk, 1) == 0);
        }
    }
    teardown(fs);
    printf("ok   1500 random writes, checkpoints, %d rollbacks and deletes; btrfs check every 100\n", rollbacks);
}

int main(void)
{
    test_basics();
    test_go_back();
    test_timer();
    test_random();
    printf("checkpoint: all tests passed\n");
    return 0;
}
