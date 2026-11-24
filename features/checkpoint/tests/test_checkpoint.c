/*
 * test_checkpoint.c - checks the checkpoint logic (by name, by position,
 * at fixed times), then runs thousands of random
 * operations against a simple model and recounts every reference after
 * each one.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkpoint.h"
#include "fs.h"
#include "mem_disk.h"
#include "volume.h"

#define CHECK(c)                                                             \
    do {                                                                     \
        if (!(c)) {                                                          \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static int page_is(Volume *v, const char *ckpt, uint32_t page, const char *want)
{
    char buf[BLOCK_SIZE];
    int r = ckpt ? ckpt_read(v, ckpt, page, buf, sizeof buf) : vol_read(v, page, buf, sizeof buf);
    if (!want)
        return r == -ENOENT;
    return r == 0 && strcmp(buf, want) == 0;
}

static void test_basics(void)
{
    Disk *d = mem_disk_create(64);
    Volume *v = vol_create(d);

    CHECK(vol_write(v, 0, "a1") == 0);
    CHECK(vol_write(v, 5, "b1") == 0);
    uint32_t before = vol_used_blocks(v);

    CHECK(ckpt_create(v, "c") == 0);
    CHECK(vol_used_blocks(v) == before);               /* a checkpoint costs nothing */
    CHECK(ckpt_create(v, "c") == -EEXIST);

    CHECK(vol_write(v, 0, "a2") == 0);
    CHECK(vol_erase(v, 5) == 0);
    CHECK(vol_write(v, 9, "new") == 0);
    CHECK(page_is(v, NULL, 0, "a2") && page_is(v, NULL, 5, NULL) && page_is(v, NULL, 9, "new"));
    CHECK(page_is(v, "c", 0, "a1") && page_is(v, "c", 5, "b1") && page_is(v, "c", 9, NULL));
    CHECK(vol_check(v) == 0);

    CHECK(ckpt_rollback(v, "c") == 0);
    CHECK(page_is(v, NULL, 0, "a1") && page_is(v, NULL, 5, "b1") && page_is(v, NULL, 9, NULL));
    CHECK(vol_check(v) == 0);

    CHECK(ckpt_delete(v, "c") == 0);
    CHECK(vol_used_blocks(v) == before);               /* back to exactly where we were */
    CHECK(ckpt_read(v, "c", 0, NULL, 0) == -ENOENT);
    CHECK(vol_check(v) == 0);

    vol_destroy(v);
    disk_close(d);
    printf("ok   create / isolate / rollback / delete\n");
}

/* Random operations, compared with a model kept in plain arrays. */
#define NCK 4
typedef char Pages[VOL_PAGES][BLOCK_SIZE]; /* "" = empty page */

/* The random tests use 16 pages, one in each of 16 different leaves. */
static uint32_t spread(int i)
{
    return (uint32_t)i * (VOL_PAGES / 16 + 1);
}

static void test_random(void)
{
    Disk *d = mem_disk_create(512);
    Volume *v = vol_create(d);
    static Pages live, snap[NCK];
    int has[NCK] = { 0 };
    char name[16];

    memset(live, 0, sizeof live);
    srand(7);
    for (int step = 0; step < 20000; step++) {
        int op = rand() % 10, k = rand() % NCK;
        uint32_t page = spread(rand() % 16);
        snprintf(name, sizeof name, "c%d", k);

        if (op < 5) {                                     /* write */
            char text[BLOCK_SIZE];
            snprintf(text, sizeof text, "step %d", step);
            CHECK(vol_write(v, page, text) == 0);
            strcpy(live[page], text);
        } else if (op < 6) {                              /* erase */
            CHECK(vol_erase(v, page) == 0);
            live[page][0] = '\0';
        } else if (op < 7) {                              /* checkpoint */
            if (has[k])
                continue;
            CHECK(ckpt_create(v, name) == 0);
            memcpy(snap[k], live, sizeof live);
            has[k] = 1;
        } else if (op < 8) {                              /* rollback */
            if (!has[k])
                continue;
            CHECK(ckpt_rollback(v, name) == 0);
            memcpy(live, snap[k], sizeof live);
        } else if (op < 9) {                              /* delete */
            if (!has[k])
                continue;
            CHECK(ckpt_delete(v, name) == 0);
            has[k] = 0;
        } else {                                          /* verify everything */
            for (uint32_t p = 0; p < VOL_PAGES; p += VOL_PAGES / 16 + 1) {
                CHECK(page_is(v, NULL, p, live[p][0] ? live[p] : NULL));
                for (int c = 0; c < NCK; c++)
                    if (has[c]) {
                        snprintf(name, sizeof name, "c%d", c);
                        CHECK(page_is(v, name, p, snap[c][p][0] ? snap[c][p] : NULL));
                    }
            }
        }
        CHECK(vol_check(v) == 0); /* every reference count right, after every step */
    }
    vol_destroy(v);
    disk_close(d);
    printf("ok   20000 random writes, erases, checkpoints, rollbacks and deletes\n");
}

/* Go back x checkpoints: x counts from the newest one. */
static void test_go_back(void)
{
    Disk *d = mem_disk_create(128);
    Volume *v = vol_create(d);
    char name[CKPT_NAME_MAX + 1], text[BLOCK_SIZE];

    CHECK(ckpt_rollback_back(v, 1) == -ENOENT);        /* no checkpoints yet */
    CHECK(ckpt_count(v) == 0);

    for (int i = 1; i <= 5; i++) {                     /* versions 1..5, one checkpoint each */
        snprintf(text, sizeof text, "version %d", i);
        snprintf(name, sizeof name, "c%d", i);
        CHECK(vol_write(v, 0, text) == 0);
        CHECK(ckpt_create(v, name) == 0);
    }
    CHECK(vol_write(v, 0, "not saved") == 0);
    CHECK(ckpt_count(v) == 5);

    CHECK(ckpt_name_back(v, 1, name) == 0 && strcmp(name, "c5") == 0);
    CHECK(ckpt_name_back(v, 5, name) == 0 && strcmp(name, "c1") == 0);
    CHECK(ckpt_name_back(v, 6, name) == -ENOENT);

    CHECK(ckpt_rollback_back(v, 1) == 0);              /* the most recent checkpoint */
    CHECK(page_is(v, NULL, 0, "version 5"));
    CHECK(ckpt_rollback_back(v, 4) == 0);              /* four back */
    CHECK(page_is(v, NULL, 0, "version 2"));
    CHECK(ckpt_rollback_back(v, 2) == 0);              /* nothing was deleted: forward again */
    CHECK(page_is(v, NULL, 0, "version 4"));
    CHECK(ckpt_rollback_back(v, 0) == -ENOENT);
    CHECK(ckpt_rollback_back(v, 6) == -ENOENT);        /* only 5 exist */
    CHECK(page_is(v, NULL, 0, "version 4"));           /* a failed rollback changes nothing */

    CHECK(ckpt_delete(v, "c4") == 0);                  /* positions close up after a delete */
    CHECK(ckpt_name_back(v, 2, name) == 0 && strcmp(name, "c3") == 0);
    CHECK(ckpt_create(v, "late") == 0);                /* a new one is always position 1 */
    CHECK(ckpt_name_back(v, 1, name) == 0 && strcmp(name, "late") == 0);
    CHECK(vol_check(v) == 0);

    vol_destroy(v);
    disk_close(d);
    printf("ok   go back x checkpoints\n");
}

/* Checkpoints at fixed times, with a clock the test controls. */
static void test_timer(void)
{
    Disk *d = mem_disk_create(128);
    Volume *v = vol_create(d);
    char name[CKPT_NAME_MAX + 1], text[BLOCK_SIZE];

    CHECK(ckpt_timer_create(v, 0, 3, 0) == NULL);      /* interval 0 makes no sense */
    CHECK(ckpt_timer_create(v, 10, 0, 0) == NULL);

    CkptTimer *t = ckpt_timer_create(v, 10, 3, 4);     /* every 10, keep 3, clock starts at 4 */
    CHECK(t != NULL && ckpt_timer_next(t) == 10);
    CHECK(ckpt_create(v, "by-hand") == 0);

    int taken = 0;
    for (uint64_t now = 5; now <= 45; now++) {         /* one tick per time unit */
        snprintf(text, sizeof text, "written at %d", (int)now);
        CHECK(vol_write(v, 0, text) == 0);
        int r = ckpt_timer_tick(t, now);
        CHECK(r == (now % 10 == 0));                   /* exactly at 10, 20, 30, 40 */
        taken += r;
        CHECK(vol_check(v) == 0);
    }
    CHECK(taken == 4);
    CHECK(ckpt_timer_next(t) == 50);

    /* keep = 3: auto-10 was deleted when auto-40 was taken; by-hand stays. */
    CHECK(ckpt_count(v) == 4);
    CHECK(page_is(v, "auto-10", 0, NULL));
    CHECK(page_is(v, "auto-20", 0, "written at 20"));
    CHECK(page_is(v, "auto-40", 0, "written at 40"));
    CHECK(page_is(v, "by-hand", 0, NULL));
    CHECK(ckpt_name_back(v, 1, name) == 0 && strcmp(name, "auto-40") == 0);

    CHECK(ckpt_rollback_back(v, 2) == 0);              /* two automatic checkpoints back */
    CHECK(page_is(v, NULL, 0, "written at 30"));

    /* A late tick: times 50 and 60 were missed, one checkpoint covers them. */
    CHECK(ckpt_timer_tick(t, 67) == 1);
    CHECK(ckpt_name_back(v, 1, name) == 0 && strcmp(name, "auto-60") == 0);
    CHECK(ckpt_timer_next(t) == 70);
    CHECK(ckpt_timer_tick(t, 69) == 0);
    CHECK(vol_check(v) == 0);

    ckpt_timer_destroy(t);
    vol_destroy(v);
    disk_close(d);
    printf("ok   checkpoints at fixed times, keeping the newest 3\n");
}

/* Checkpoints of a whole filesystem: files, directories and the inode
 * free list all come back on rollback. */
static int fs_has(Fs *fs, const char *path, const char *want)
{
    char buf[FS_MAX_FILE + 1];
    int n = fs_read(fs, path, 0, buf, FS_MAX_FILE);
    if (!want)
        return n == -ENOENT;
    if (n < 0)
        return 0;
    buf[n] = '\0';
    return strcmp(buf, want) == 0;
}

static void test_files(void)
{
    Disk *d = mem_disk_create(1024);
    Volume *v = vol_create(d);
    FsInfo before, after;
    FsStat st;

    CHECK(fs_format(v) == 0);
    Fs *fs = fs_open(v);
    CHECK(fs_mkdir(fs, "/docs") == 0);
    CHECK(fs_create(fs, "/docs/report.txt") == 0);
    CHECK(fs_write(fs, "/docs/report.txt", 0, "draft", 5) == 5);
    CHECK(fs_info(fs, &before) == 0);

    CHECK(ckpt_create(v, "monday") == 0);             /* the whole filesystem, 0 copies */

    CHECK(fs_write(fs, "/docs/report.txt", 0, "FINAL", 5) == 5);
    CHECK(fs_create(fs, "/docs/new.txt") == 0);        /* takes an inode off the free list */
    CHECK(fs_unlink(fs, "/docs/report.txt") == 0);     /* gives one back */
    CHECK(fs_mkdir(fs, "/tmp") == 0);
    CHECK(fs_check(fs) == 0);

    Fs *old = ckpt_open_fs(v, "monday");               /* look inside the checkpoint */
    CHECK(old != NULL);
    CHECK(fs_has(old, "/docs/report.txt", "draft"));
    CHECK(fs_has(old, "/docs/new.txt", NULL));
    CHECK(fs_stat(old, "/tmp", &st) == -ENOENT);
    CHECK(fs_create(old, "/x") == -EROFS);             /* read-only */
    CHECK(fs_check(old) == 0);
    fs_close(old);

    CHECK(ckpt_rollback(v, "monday") == 0);            /* back to monday, everything */
    CHECK(fs_has(fs, "/docs/report.txt", "draft"));
    CHECK(fs_has(fs, "/docs/new.txt", NULL));
    CHECK(fs_stat(fs, "/tmp", &st) == -ENOENT);
    CHECK(fs_info(fs, &after) == 0);
    CHECK(after.inodes_free == before.inodes_free);    /* free inode list restored too */
    CHECK(after.blocks_free == before.blocks_free);
    CHECK(fs_check(fs) == 0);
    CHECK(vol_check(v) == 0);

    fs_close(fs);
    vol_destroy(v);
    disk_close(d);
    printf("ok   checkpoints of a filesystem: read an old file, roll back files and inodes\n");
}

int main(void)
{
    test_files();
    test_basics();
    test_go_back();
    test_timer();
    test_random();
    printf("all tests passed\n");
    return 0;
}
