/*
 * demo_recovery.c - commit, crash, and come back.
 *
 * The volume lives in a file (recovery_demo.img), so "the disk" really
 * outlives the Volume object. A crash is simulated by throwing the Volume
 * away without committing and opening the file again.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkpoint.h"
#include "file_disk.h"
#include "recovery.h"
#include "volume.h"

#define IMAGE "recovery_demo.img"
#define BLOCKS 128

static void show(Volume *vol)
{
    char text[BLOCK_SIZE], name[CKPT_NAME_MAX + 1];
    for (uint32_t p = 0; p < VOL_PAGES; p++)
        if (vol_read(vol, p, text, sizeof text) == 0)
            printf("  page %-2u \"%s\"\n", p, text);
    for (uint32_t x = 1; ckpt_name_back(vol, x, name) == 0; x++) {
        ckpt_read(vol, name, 0, text, sizeof text);
        printf("  checkpoint \"%s\": page 0 = \"%s\"\n", name, text);
    }
    printf("  reference counts: %s\n", vol_check(vol) == 0 ? "OK" : "WRONG");
}

static void must(int r, const char *what)
{
    if (r < 0) {
        fprintf(stderr, "%s failed: %s\n", what, strerror(-r));
        exit(1);
    }
}

/* Lose everything in memory, keep only the file, and mount it again. */
static Volume *crash(Volume *vol, Disk **disk)
{
    vol_destroy(vol);
    disk_close(*disk);
    *disk = file_disk_open(IMAGE, BLOCKS);
    vol = NULL;
    if (!*disk)
        exit(1);
    must(rec_mount(*disk, &vol), "mount");
    printf("  ** crash **  mounted commit %u from the file\n", rec_generation(*disk));
    return vol;
}

int main(void)
{
    Disk *disk;
    Volume *vol;
    char junk[BLOCK_SIZE];

    remove(IMAGE);
    disk = file_disk_open(IMAGE, BLOCKS);
    vol = disk ? vol_create(disk) : NULL;
    if (!vol)
        return 1;

    printf("==== 1. Write, take a checkpoint, write again, COMMIT ====\n");
    must(vol_write(vol, 0, "balance: 100"), "write");
    must(ckpt_create(vol, "monday"), "checkpoint");
    must(vol_write(vol, 0, "balance: 250"), "write");
    must(vol_write(vol, 1, "note: paid in 150"), "write");
    must(rec_commit(vol), "commit");
    show(vol);

    printf("\n==== 2. More changes, NOT committed ====\n");
    must(vol_write(vol, 0, "balance: 999999"), "write");
    must(vol_erase(vol, 1), "erase");
    must(ckpt_delete(vol, "monday"), "delete");
    show(vol);

    printf("\n==== 3. Crash before the next commit ====\n");
    vol = crash(vol, &disk);
    show(vol);
    printf("-> exactly the state of step 1. The uncommitted changes are gone, and the\n"
           "   checkpoint deleted in step 2 is back.\n");

    printf("\n==== 4. A commit that is cut off halfway ====\n");
    must(vol_write(vol, 0, "balance: 300"), "write");
    memset(junk, 0x5a, sizeof junk);
    disk_write(disk, 1, junk); /* commit 2 would write blocks 1-8; only block 1 got out */
    printf("  commit 2 started writing its superblock, then the power went\n");
    vol = crash(vol, &disk);
    show(vol);
    printf("-> the damaged superblock fails its checksum, so mount used commit 1.\n");

    printf("\n==== 5. Commit properly, crash again ====\n");
    must(vol_write(vol, 0, "balance: 300"), "write");
    must(rec_commit(vol), "commit");
    vol = crash(vol, &disk);
    show(vol);

    vol_destroy(vol);
    disk_close(disk);
    remove(IMAGE);
    return 0;
}
