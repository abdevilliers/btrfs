/*
 * demo_timed_checkpoints.c - checkpoints taken automatically at fixed
 * times, then "go back x checkpoints".
 *
 *     demo_timed_checkpoints [x]        x = how many checkpoints to go back
 *                                       (default 3)
 *
 * The demo uses its own clock (a counter) so it finishes at once and
 * always prints the same thing. In a real program, pass time(NULL) to
 * ckpt_timer_tick instead and the checkpoints happen in real seconds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkpoint.h"
#include "mem_disk.h"
#include "volume.h"

#define INTERVAL 10 /* a checkpoint every 10 time units */
#define KEEP 5      /* keep the newest 5                */

static void list(Volume *vol)
{
    char name[CKPT_NAME_MAX + 1], text[BLOCK_SIZE];
    printf("  x   checkpoint   page 0 in it\n");
    for (uint32_t x = 1; ckpt_name_back(vol, x, name) == 0; x++) {
        ckpt_read(vol, name, 0, text, sizeof text);
        printf("  %-3u %-12s \"%s\"\n", x, name, text);
    }
}

int main(int argc, char **argv)
{
    uint32_t x = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 10) : 3;
    Disk *disk = mem_disk_create(128);
    Volume *vol = vol_create(disk);
    CkptTimer *timer = ckpt_timer_create(vol, INTERVAL, KEEP, 0);
    char text[BLOCK_SIZE];

    if (!disk || !vol || !timer)
        return 1;

    printf("==== 1. Work for 60 time units; a checkpoint every %d, keep %d ====\n", INTERVAL, KEEP);
    for (uint64_t now = 1; now <= 60; now++) {
        if (now % 7 == 0) { /* the user saves page 0 now and then */
            snprintf(text, sizeof text, "saved at t=%d", (int)now);
            vol_write(vol, 0, text);
            printf("t=%-3d write page 0: \"%s\"\n", (int)now, text);
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
    vol_read(vol, 0, text, sizeof text);
    printf("before: page 0 = \"%s\"\n", text);
    int r = ckpt_rollback_back(vol, x);
    if (r) {
        printf("cannot go back %u: there are only %u checkpoints\n", x, ckpt_count(vol));
    } else {
        ckpt_name_back(vol, x, text);
        printf("rolled back to \"%s\"\n", text);
        vol_read(vol, 0, text, sizeof text);
        printf("after:  page 0 = \"%s\"\n", text);
    }
    printf("reference counts: %s\n", vol_check(vol) == 0 ? "OK" : "WRONG");

    ckpt_timer_destroy(timer);
    vol_destroy(vol);
    disk_close(disk);
    return r ? 1 : 0;
}
