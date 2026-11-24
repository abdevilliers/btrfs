/*
 * test_recovery.c - after a "crash" the volume must come back exactly as
 * it was at the last commit.
 *
 * A crash is simulated by throwing the Volume away (vol_destroy) without
 * committing: everything it kept in memory is lost, and only the blocks
 * on the disk remain, as after a power cut.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkpoint.h"
#include "file_disk.h"
#include "fs.h"
#include "mem_disk.h"
#include "recovery.h"
#include "volume.h"

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

static int page_is(Volume *v, const char *ckpt, uint32_t page, const char *want)
{
    char buf[BLOCK_SIZE];
    int r = ckpt ? ckpt_read(v, ckpt, page, buf, sizeof buf) : vol_read(v, page, buf, sizeof buf);
    return want ? (r == 0 && strcmp(buf, want) == 0) : r == -ENOENT;
}

static Volume *crash_and_mount(Volume *v, Disk *d)
{
    vol_destroy(v); /* the crash */
    v = NULL;
    CHECK(rec_mount(d, &v) == 0 && v != NULL);
    CHECK(vol_check(v) == 0);
    return v;
}

static void test_commit_and_crash(void)
{
    Disk *d = mem_disk_create(128);
    Volume *v = NULL;

    CHECK(rec_mount(d, &v) == -ENOENT);                /* a blank disk has no commit */
    CHECK(rec_generation(d) == 0);
    v = vol_create(d);

    CHECK(vol_write(v, 0, "committed") == 0);
    CHECK(vol_write(v, 1, "also committed") == 0);
    CHECK(rec_commit(v) == 0);
    CHECK(rec_generation(d) == 1);

    CHECK(vol_write(v, 0, "lost") == 0);               /* all of this is after the commit */
    CHECK(vol_erase(v, 1) == 0);
    CHECK(vol_write(v, 2, "lost too") == 0);

    v = crash_and_mount(v, d);
    CHECK(page_is(v, NULL, 0, "committed"));           /* back to the commit, exactly */
    CHECK(page_is(v, NULL, 1, "also committed"));
    CHECK(page_is(v, NULL, 2, NULL));

    CHECK(vol_write(v, 2, "second") == 0);             /* life goes on after recovery */
    CHECK(rec_commit(v) == 0);
    CHECK(rec_generation(d) == 2);
    v = crash_and_mount(v, d);
    CHECK(page_is(v, NULL, 2, "second"));

    vol_destroy(v);
    disk_close(d);
    printf("ok   commit, crash, mount: the committed state is back\n");
}

static void test_checkpoints_survive(void)
{
    Disk *d = mem_disk_create(128);
    Volume *v = vol_create(d);
    char name[CKPT_NAME_MAX + 1];

    CHECK(vol_write(v, 0, "version 1") == 0);
    CHECK(ckpt_create(v, "c1") == 0);
    CHECK(vol_write(v, 0, "version 2") == 0);
    CHECK(ckpt_create(v, "c2") == 0);
    CHECK(vol_write(v, 0, "version 3") == 0);
    CHECK(rec_commit(v) == 0);

    CHECK(ckpt_create(v, "c3") == 0);                  /* not committed */
    CHECK(ckpt_delete(v, "c1") == 0);                  /* not committed either */
    CHECK(ckpt_rollback(v, "c2") == 0);

    v = crash_and_mount(v, d);
    CHECK(page_is(v, NULL, 0, "version 3"));
    CHECK(page_is(v, "c1", 0, "version 1"));           /* the delete was undone */
    CHECK(page_is(v, "c2", 0, "version 2"));
    CHECK(ckpt_count(v) == 2);                         /* c3 never happened */
    CHECK(ckpt_name_back(v, 1, name) == 0 && strcmp(name, "c2") == 0); /* order kept */

    CHECK(ckpt_rollback_back(v, 2) == 0);              /* go back 2 still works */
    CHECK(page_is(v, NULL, 0, "version 1"));
    CHECK(vol_check(v) == 0);

    vol_destroy(v);
    disk_close(d);
    printf("ok   checkpoints and their order survive a crash\n");
}

/* A commit that stops halfway leaves a damaged superblock slot. Mount
 * must ignore it and use the previous commit. Commit g uses slot g % 2:
 * slot 0 is blocks 1 .. 8, slot 1 is blocks 9 .. 16. */
static void test_interrupted_commit(void)
{
    Disk *d = mem_disk_create(128);
    Volume *v = vol_create(d);
    char junk[BLOCK_SIZE];

    memset(junk, 0x5a, sizeof junk);
    CHECK(vol_write(v, 0, "one") == 0);
    CHECK(rec_commit(v) == 0);                         /* commit 1, in slot 1 */
    CHECK(vol_write(v, 0, "two") == 0);

    /* Commit 2 starts writing slot 0 and the power goes after one block. */
    CHECK(disk_write(d, 1, junk) == 0);
    v = crash_and_mount(v, d);
    CHECK(rec_generation(d) == 1);
    CHECK(page_is(v, NULL, 0, "one"));

    /* A full commit 2, then its last block is damaged. */
    CHECK(vol_write(v, 0, "two") == 0);
    CHECK(rec_commit(v) == 0);
    CHECK(rec_generation(d) == 2);
    CHECK(disk_write(d, VOL_META_BLOCKS / 2, junk) == 0);
    v = crash_and_mount(v, d);
    CHECK(rec_generation(d) == 1);
    CHECK(page_is(v, NULL, 0, "one"));

    /* Both slots damaged: nothing to mount. */
    CHECK(disk_write(d, VOL_META_BLOCKS / 2 + 1, junk) == 0);
    vol_destroy(v);
    v = NULL;
    CHECK(rec_mount(d, &v) == -ENOENT && v == NULL);

    disk_close(d);
    printf("ok   a half-written commit is ignored\n");
}

/* The data really is in the file: close everything, open the file again. */
static void test_file_disk(void)
{
    const char *path = "test_recovery.img";
    Disk *d;
    Volume *v = NULL;

    remove(path);
    d = file_disk_open(path, 128);
    CHECK(d != NULL);
    v = vol_create(d);
    CHECK(vol_write(v, 3, "on disk") == 0);
    CHECK(ckpt_create(v, "saved") == 0);
    CHECK(rec_commit(v) == 0);
    CHECK(vol_write(v, 3, "never committed") == 0);
    vol_destroy(v);
    disk_close(d);                                     /* as if the program ended */

    d = file_disk_open(path, 128);                     /* a new run of the program */
    CHECK(d != NULL);
    v = NULL;
    CHECK(rec_mount(d, &v) == 0);
    CHECK(page_is(v, NULL, 3, "on disk"));
    CHECK(page_is(v, "saved", 3, "on disk"));
    CHECK(vol_check(v) == 0);
    vol_destroy(v);
    disk_close(d);
    remove(path);
    printf("ok   state survives closing and reopening a file-backed disk\n");
}

/* Random work with commits and crashes at random moments, compared with
 * a model: `live`/`snap` is the state now, `c_live`/`c_snap` the state at
 * the last commit. */
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
    static Pages live, snap[NCK], c_live, c_snap[NCK];
    int has[NCK] = { 0 }, c_has[NCK] = { 0 };
    int commits = 0, crashes = 0;
    char name[16];

    CHECK(rec_commit(v) == 0); /* commit the empty volume, so there is always one */
    srand(3);
    for (int step = 0; step < 20000; step++) {
        int op = rand() % 20, k = rand() % NCK;
        uint32_t page = spread(rand() % 16);
        snprintf(name, sizeof name, "c%d", k);

        if (op < 10) {                                    /* write */
            char text[BLOCK_SIZE];
            snprintf(text, sizeof text, "step %d", step);
            CHECK(vol_write(v, page, text) == 0);
            strcpy(live[page], text);
        } else if (op < 12) {                             /* erase */
            CHECK(vol_erase(v, page) == 0);
            live[page][0] = '\0';
        } else if (op < 14) {                             /* checkpoint */
            if (!has[k]) {
                CHECK(ckpt_create(v, name) == 0);
                memcpy(snap[k], live, sizeof live);
                has[k] = 1;
            }
        } else if (op < 15) {                             /* rollback */
            if (has[k]) {
                CHECK(ckpt_rollback(v, name) == 0);
                memcpy(live, snap[k], sizeof live);
            }
        } else if (op < 16) {                             /* delete */
            if (has[k]) {
                CHECK(ckpt_delete(v, name) == 0);
                has[k] = 0;
            }
        } else if (op < 18) {                             /* commit */
            CHECK(rec_commit(v) == 0);
            memcpy(c_live, live, sizeof live);
            memcpy(c_snap, snap, sizeof snap);
            memcpy(c_has, has, sizeof has);
            commits++;
        } else {                                          /* crash */
            v = crash_and_mount(v, d);
            memcpy(live, c_live, sizeof live);
            memcpy(snap, c_snap, sizeof snap);
            memcpy(has, c_has, sizeof has);
            crashes++;
        }

        for (uint32_t p = 0; p < VOL_PAGES; p += VOL_PAGES / 16 + 1) {
            CHECK(page_is(v, NULL, p, live[p][0] ? live[p] : NULL));
            for (int c = 0; c < NCK; c++)
                if (has[c]) {
                    snprintf(name, sizeof name, "c%d", c);
                    CHECK(page_is(v, name, p, snap[c][p][0] ? snap[c][p] : NULL));
                }
        }
        CHECK(vol_check(v) == 0);
    }
    vol_destroy(v);
    disk_close(d);
    printf("ok   20000 random operations with %d commits and %d crashes\n", commits, crashes);
}

/* Files and directories come back after a crash, and the filesystem
 * is consistent (fsck clean). */
static void test_files_survive(void)
{
    Disk *d = mem_disk_create(1024);
    Volume *v = vol_create(d);
    char buf[64];
    FsStat st;

    CHECK(fs_format(v) == 0);
    Fs *fs = fs_open(v);
    CHECK(fs_mkdir(fs, "/home") == 0);
    CHECK(fs_create(fs, "/home/a.txt") == 0);
    CHECK(fs_write(fs, "/home/a.txt", 0, "kept", 4) == 4);
    CHECK(rec_commit(v) == 0);

    CHECK(fs_write(fs, "/home/a.txt", 0, "LOST", 4) == 4); /* after the commit */
    CHECK(fs_create(fs, "/home/b.txt") == 0);
    CHECK(fs_rmdir(fs, "/home") == -ENOTEMPTY);
    fs_close(fs);

    v = crash_and_mount(v, d);
    fs = fs_open(v);
    CHECK(fs != NULL);
    CHECK(fs_read(fs, "/home/a.txt", 0, buf, sizeof buf) == 4 && memcmp(buf, "kept", 4) == 0);
    CHECK(fs_stat(fs, "/home/b.txt", &st) == -ENOENT);
    CHECK(fs_check(fs) == 0);
    CHECK(fs_create(fs, "/home/b.txt") == 0);              /* and it keeps working */

    fs_close(fs);
    vol_destroy(v);
    disk_close(d);
    printf("ok   files and directories survive a crash; fsck is clean after mount\n");
}

int main(void)
{
    test_files_survive();
    test_commit_and_crash();
    test_checkpoints_survive();
    test_interrupted_commit();
    test_file_disk();
    test_random();
    printf("recovery: all tests passed\n");
    return 0;
}
