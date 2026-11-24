/*
 * demo_timed_checkpoints.c - checkpoints taken automatically at fixed
 * times while a file is being updated, then "go back x checkpoints".
 *
 *     demo_timed_checkpoints [x]        x = how many checkpoints to go back
 *                                       (default 3)
 *
 * The demo uses its own clock (a counter) so it finishes at once and
 * always prints the same thing. In a real program, pass time(NULL) to
 * ckpt_timer_tick and the checkpoints happen in real seconds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkpoint.h"
#include "fs.h"
#include "mem_disk.h"
#include "volume.h"

#define INTERVAL 10 /* a checkpoint every 10 time units */
#define KEEP 5      /* keep the newest 5                */

static void read_file(Fs *fs, char *buf)
{
    int n = fs_read(fs, "/status.txt", 0, buf, FS_MAX_FILE);
    buf[n > 0 ? n : 0] = '\0';
}

static void list(Volume *vol)
{
    char name[CKPT_NAME_MAX + 1], text[FS_MAX_FILE + 1];
    printf("  x   checkpoint   /status.txt in it\n");
    for (uint32_t x = 1; ckpt_name_back(vol, x, name) == 0; x++) {
        Fs *old = ckpt_open_fs(vol, name);
        read_file(old, text);
        fs_close(old);
        printf("  %-3u %-12s \"%s\"\n", x, name, text);
    }
}

int main(int argc, char **argv)
{
    uint32_t x = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 3;
    Disk *disk = mem_disk_create(2048);
    Volume *vol = disk ? vol_create(disk) : NULL;
    Fs *fs;
    char text[FS_MAX_FILE + 1];

    if (!vol || fs_format(vol) < 0 || !(fs = fs_open(vol)) || fs_create(fs, "/status.txt") < 0)
        return 1;
    CkptTimer *timer = ckpt_timer_create(vol, INTERVAL, KEEP, 0);
    if (!timer)
        return 1;

    printf("==== 1. Work for 60 time units; a checkpoint every %d, keep %d ====\n", INTERVAL, KEEP);
    for (uint64_t now = 1; now <= 60; now++) {
        if (now % 7 == 0) { /* the program updates its file now and then */
            snprintf(text, sizeof text, "saved at t=%d", (int)now);
            fs_truncate(fs, "/status.txt", 0);
            fs_write(fs, "/status.txt", 0, text, (uint32_t)strlen(text));
            printf("t=%-3d write /status.txt: \"%s\"\n", (int)now, text);
        }
        if (ckpt_timer_tick(timer, now) == 1) {
            ckpt_name_back(vol, 1, text);
            printf("t=%-3d checkpoint \"%s\" taken automatically\n", (int)now, text);
        }
    }

    printf("\n==== 2. The checkpoints, newest first ====\n");
    list(vol);
    printf("-> auto-10 is gone: only the newest %d are kept.\n", KEEP);

    printf("\n==== 3. Go back %u checkpoint(s) ====\n", x);
    read_file(fs, text);
    printf("before: /status.txt = \"%s\"\n", text);
    int r = ckpt_rollback_back(vol, x);
    if (r) {
        printf("cannot go back %u: there are only %u checkpoints\n", x, ckpt_count(vol));
    } else {
        ckpt_name_back(vol, x, text);
        printf("rolled back to \"%s\"\n", text);
        read_file(fs, text);
        printf("after:  /status.txt = \"%s\"\n", text);
    }
    printf("fsck: %s   reference counts: %s\n", fs_check(fs) ? "PROBLEMS" : "clean",
           vol_check(vol) == 0 ? "OK" : "WRONG");

    ckpt_timer_destroy(timer);
    fs_close(fs);
    vol_destroy(vol);
    disk_close(disk);
    return r ? 1 : 0;
}
