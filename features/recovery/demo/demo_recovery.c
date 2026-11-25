/*
 * demo_recovery.c - a filesystem that survives a crash.
 *
 * The disk is a file (recovery_demo.img), so it outlives the program.
 * A crash throws away everything in memory without committing and opens
 * the image again. Then: a commit cut off by a power failure, and a
 * damaged superblock.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fault_disk.h"
#include "file_disk.h"
#include "fs.h"
#include "recovery.h"

#define IMAGE "recovery_demo.img"
#define BLOCKS 20000 /* 78 MiB: room for the superblock copy at 64 MiB */

static Disk *disk;
static Btrfs *b;
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
    if (r < 0 && r != -EEXIST)
        must(r, "create");
    must(fs_truncate(fs, path, 0), "truncate");
    must((int)fs_write(fs, path, 0, text, strlen(text)), "write");
}

static void entry(void *arg, const char *name, const FsStat *st)
{
    char path[64], buf[64];
    if (st->type != FS_FILE)
        return;
    snprintf(path, sizeof path, "%s/%s", (const char *)arg, name);
    int64_t n = fs_read(fs, path, 0, buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = '\0';
    printf("  %-22s \"%s\"\n", path, buf);
}

static void show(void)
{
    fs_readdir(fs, "/bank", entry, "/bank");
    printf("  last commit on disk: generation %llu\n", (unsigned long long)rec_generation(disk));
}

/* Lose everything in memory; keep only the image file; mount it. */
static void crash(void)
{
    const char *how[] = { "a plain mount", "-o usebackuproot", "btrfs rescue super-recover" };
    int h;
    fs_close(fs);
    btrfs_crash(b);
    disk_close(disk);
    disk = file_disk_open(IMAGE, BLOCKS);
    if (!disk)
        exit(1);
    must(rec_mount(disk, &b, &h), "mount");
    fs = fs_open(b);
    printf("  ** crash **  mounted generation %llu (%s); btrfs check: %s\n", (unsigned long long)btrfs_generation(b),
           how[h], btrfs_check(disk, 0) ? "PROBLEMS" : "clean");
}

int main(void)
{
    BtrfsMkfsOpts o = { 4096, NULL };
    uint8_t junk[BLOCK_SIZE];

    remove(IMAGE);
    disk = file_disk_open(IMAGE, BLOCKS);
    if (!disk || btrfs_mkfs(disk, &o) < 0 || btrfs_mount(disk, NULL, &b) < 0 || !(fs = fs_open(b)))
        return 1;

    printf("==== 1. Make files, COMMIT ====\n");
    must(fs_mkdir(fs, "/bank"), "mkdir");
    put("/bank/balance.txt", "250");
    put("/bank/log.txt", "paid in 150");
    must(rec_commit(b), "commit");
    show();

    printf("\n==== 2. More changes, NOT committed, then a crash ====\n");
    put("/bank/balance.txt", "999999");
    must(fs_unlink(fs, "/bank/log.txt"), "unlink");
    put("/bank/hack.txt", "oops");
    show();
    crash();
    show();
    printf("-> exactly the state of step 1.\n");

    printf("\n==== 3. A commit cut off by a power failure ====\n");
    put("/bank/balance.txt", "300");
    fs_close(fs);
    btrfs_crash(b);
    Disk *fd = fault_disk_wrap(disk);
    must(btrfs_mount(fd, NULL, &b), "mount");
    fs = fs_open(b);
    put("/bank/balance.txt", "300");
    fault_disk_cut_after(fd, 4); /* the power goes after 4 writes of the commit */
    rec_commit(b);
    printf("  the commit wrote 4 blocks; %llu more never reached the disk\n",
           (unsigned long long)fault_disk_dropped(fd));
    fs_close(fs);
    btrfs_crash(b);
    disk_close(fd);
    fs = NULL;
    b = NULL;
    {
        int h;
        must(rec_mount(disk, &b, &h), "mount");
        fs = fs_open(b);
        printf("  ** power back **  mounted generation %llu; btrfs check: %s\n",
               (unsigned long long)btrfs_generation(b), btrfs_check(disk, 0) ? "PROBLEMS" : "clean");
    }
    show();
    printf("-> some new tree blocks reached the disk, but not the superblock that would\n"
           "   point at them; the old commit was never overwritten (copy-on-write).\n");

    printf("\n==== 4. The primary superblock is damaged ====\n");
    put("/bank/balance.txt", "300");
    must(rec_commit(b), "commit");
    memset(junk, 0x5a, sizeof junk);
    disk_write(disk, 16, junk); /* the superblock at 64 KiB */
    printf("  the superblock at 64 KiB is now garbage\n");
    crash();
    show();
    printf("-> the mirror at 64 MiB was copied back over it.\n");

    fs_close(fs);
    btrfs_unmount(b);
    disk_close(disk);
    remove(IMAGE);
    return 0;
}
