/*
 * demo_timed_checkpoints.c - checkpoints taken automatically at fixed
 * times while a file is being updated, then "go back x checkpoints".
 *
 *     demo_timed_checkpoints [x]        x = how many to go back (default 3)
 *
 * The demo uses its own clock (a counter) so it finishes at once. In a
 * real program, pass time(NULL) to ckpt_timer_tick.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkpoint.h"
#include "fs.h"
#include "mem_disk.h"

#define INTERVAL 10
#define KEEP 5

static void read_file(Fs *fs, char *buf)
{
    int64_t n = fs_read(fs, "/status.txt", 0, buf, 63);
    buf[n > 0 ? n : 0] = '\0';
}

int main(int argc, char **argv)
{
    uint32_t x = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 3;
    BtrfsMkfsOpts o = { 4096, NULL };
    Disk *disk = mem_disk_create(16384);
    Btrfs *b;
    char text[64], name[CKPT_NAME_MAX + 1];

    if (!disk || btrfs_mkfs(disk, &o) < 0 || btrfs_mount(disk, NULL, &b) < 0 || ckpt_setup(b) < 0)
        return 1;
    Fs *fs = fs_open(b);
    if (!fs || fs_create(fs, "/status.txt") < 0)
        return 1;
    CkptTimer *timer = ckpt_timer_create(b, INTERVAL, KEEP, 0);

    printf("==== 1. Work for 60 time units; a checkpoint every %d, keep %d ====\n", INTERVAL, KEEP);
    for (uint64_t now = 1; now <= 60; now++) {
        if (now % 7 == 0) {
            snprintf(text, sizeof text, "saved at t=%d", (int)now);
            fs_truncate(fs, "/status.txt", 0);
            fs_write(fs, "/status.txt", 0, text, strlen(text));
            printf("t=%-3d write /status.txt: \"%s\"\n", (int)now, text);
        }
        if (ckpt_timer_tick(timer, now) == 1) {
            ckpt_name_back(b, 1, name);
            printf("t=%-3d checkpoint \"%s\" taken automatically\n", (int)now, name);
        }
    }

    printf("\n==== 2. The checkpoints, newest first ====\n  x   checkpoint   /status.txt in it\n");
    for (uint32_t i = 1; ckpt_name_back(b, i, name) == 0; i++) {
        Fs *old = ckpt_open_fs(b, name);
        read_file(old, text);
        fs_close(old);
        printf("  %-3u %-12s \"%s\"\n", i, name, text);
    }
    printf("-> auto-10 is gone: only the newest %d are kept.\n", KEEP);

    printf("\n==== 3. Go back %u checkpoint(s) ====\n", x);
    read_file(fs, text);
    printf("before: /status.txt = \"%s\"\n", text);
    fs_close(fs);
    int r = ckpt_rollback_back(b, x);
    fs = fs_open(b);
    if (r) {
        printf("cannot go back %u: there are only %u checkpoints\n", x, ckpt_count(b));
    } else {
        ckpt_name_back(b, x, name);
        read_file(fs, text);
        printf("rolled back to \"%s\"\nafter:  /status.txt = \"%s\"\n", name, text);
    }
    btrfs_commit(b);
    printf("btrfs check: %s\n", btrfs_check(disk, 0) ? "PROBLEMS" : "clean");

    ckpt_timer_destroy(timer);
    fs_close(fs);
    btrfs_unmount(b);
    disk_close(disk);
    return r ? 1 : 0;
}
