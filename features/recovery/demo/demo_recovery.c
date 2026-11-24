/*
 * demo_recovery.c - a filesystem that survives a crash.
 *
 * The volume lives in a file (recovery_demo.img), so the "disk" outlives
 * the program. Files are written with the normal file operations; a crash
 * is simulated by throwing everything in memory away without committing
 * and opening the image again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkpoint.h"
#include "file_disk.h"
#include "fs.h"
#include "recovery.h"
#include "volume.h"

#define IMAGE "recovery_demo.img"
#define BLOCKS 2048

static Disk *disk;
static Volume *vol;
static Fs *fs;

static void must(int r, const char *what)
{
    if (r < 0) {
        fprintf(stderr, "%s failed: %s\n", what, strerror(-r));
        exit(1);
    }
}

static void put(const char *path, const char *text)
{
    int r = fs_create(fs, path);
    if (r < 0 && r != -17 /* EEXIST */)
        must(r, "create");
    must(fs_truncate(fs, path, 0), "truncate");
    must(fs_write(fs, path, 0, text, (uint32_t)strlen(text)), "write");
}

static void entry(void *arg, const char *name, const FsStat *st)
{
    char path[64], buf[FS_MAX_FILE + 1];
    if (st->type != FS_FILE)
        return;
    snprintf(path, sizeof path, "%s/%s", (const char *)arg, name);
    int n = fs_read(fs, path, 0, buf, FS_MAX_FILE);
    buf[n > 0 ? n : 0] = '\0';
    printf("  %-22s \"%s\"\n", path, buf);
}

static void show(void)
{
    char name[CKPT_NAME_MAX + 1];
    fs_readdir(fs, "/bank", entry, "/bank");
    for (uint32_t x = 1; ckpt_name_back(vol, x, name) == 0; x++)
        printf("  checkpoint \"%s\"\n", name);
    printf("  fsck: %s   reference counts: %s\n", fs_check(fs) ? "PROBLEMS" : "clean",
           vol_check(vol) ? "WRONG" : "OK");
}

/* Lose everything in memory, keep only the image file, and mount it. */
static void crash(void)
{
    fs_close(fs);
    vol_destroy(vol);
    disk_close(disk);
    disk = file_disk_open(IMAGE, BLOCKS);
    if (!disk)
        exit(1);
    must(rec_mount(disk, &vol), "mount");
    fs = fs_open(vol);
    if (!fs)
        exit(1);
    printf("  ** crash **  mounted commit %u from the file\n", rec_generation(disk));
}

int main(void)
{
    char junk[BLOCK_SIZE];

    remove(IMAGE);
    disk = file_disk_open(IMAGE, BLOCKS);
    vol = disk ? vol_create(disk) : NULL;
    if (!vol || fs_format(vol) < 0 || !(fs = fs_open(vol)))
        return 1;

    printf("==== 1. Make files, take a checkpoint, update, COMMIT ====\n");
    must(fs_mkdir(fs, "/bank"), "mkdir");
    put("/bank/balance.txt", "100");
    must(ckpt_create(vol, "monday"), "checkpoint");
    put("/bank/balance.txt", "250");
    put("/bank/log.txt", "paid in 150");
    must(rec_commit(vol), "commit");
    show();

    printf("\n==== 2. More changes, NOT committed ====\n");
    put("/bank/balance.txt", "999999");
    must(fs_unlink(fs, "/bank/log.txt"), "unlink");
    put("/bank/hack.txt", "oops");
    must(ckpt_delete(vol, "monday"), "delete");
    show();

    printf("\n==== 3. Crash before the next commit ====\n");
    crash();
    show();
    printf("-> exactly the state of step 1: files, inodes and free lists as committed,\n"
           "   and the deleted checkpoint is back.\n");

    printf("\n==== 4. A commit that is cut off halfway ====\n");
    put("/bank/balance.txt", "300");
    memset(junk, 0x5a, sizeof junk);
    disk_write(disk, 1, junk); /* commit 2 would write blocks 1-8; only block 1 got out */
    printf("  commit 2 started writing its superblock, then the power went\n");
    crash();
    show();
    printf("-> the damaged superblock fails its checksum, so mount used commit 1.\n");

    printf("\n==== 5. Commit properly, crash again ====\n");
    put("/bank/balance.txt", "300");
    must(rec_commit(vol), "commit");
    crash();
    show();

    fs_close(fs);
    vol_destroy(vol);
    disk_close(disk);
    remove(IMAGE);
    return 0;
}
