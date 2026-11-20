/*
 * demo.c - walks through checkpointing step by step, printing the trees
 * and every block's reference count after each step.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkpoint.h"
#include "mem_disk.h"
#include "volume.h"

static Disk *disk;
static Volume *vol;
static uint32_t used_before;
static uint64_t writes_before;

static void step(const char *title)
{
    printf("\n==== %s ====\n", title);
    used_before = vol_used_blocks(vol);
    writes_before = disk_write_count(disk);
}

static void show(void)
{
    vol_print(vol);
    printf("blocks written: %llu   blocks in use: %u (%+d)   reference counts: %s\n",
           (unsigned long long)(disk_write_count(disk) - writes_before), vol_used_blocks(vol),
           (int)vol_used_blocks(vol) - (int)used_before, vol_check(vol) == 0 ? "OK" : "WRONG");
}

static void must(int r, const char *what)
{
    if (r < 0) {
        fprintf(stderr, "%s failed: %s\n", what, strerror(-r));
        exit(1);
    }
}

int main(void)
{
    char buf[BLOCK_SIZE];

    disk = mem_disk_create(64);
    vol = vol_create(disk);
    if (!disk || !vol)
        return 1;

    step("1. Write four pages");
    must(vol_write(vol, 0, "intro, draft"), "write");
    must(vol_write(vol, 1, "chapter 1, draft"), "write");
    must(vol_write(vol, 4, "chapter 2, draft"), "write");
    must(vol_write(vol, 5, "chapter 3, draft"), "write");
    show();

    step("2. Take checkpoint \"v1\"");
    must(ckpt_create(vol, "v1"), "checkpoint");
    show();
    printf("-> nothing was copied: the checkpoint points at the same root, which now has 2 references.\n");

    step("3. Change page 1 in the live volume");
    must(vol_write(vol, 1, "chapter 1, FINAL"), "write");
    show();
    printf("-> the root and the leaf for pages 0-3 were copied. The leaf for pages 4-7\n"
           "   is now shared by both trees. Old page 1 is kept for the checkpoint.\n");

    step("4. Erase page 4 in the live volume");
    must(vol_erase(vol, 4), "erase");
    show();

    step("5. Read page 1 from both");
    vol_read(vol, 1, buf, sizeof buf);
    printf("live volume:     \"%s\"\n", buf);
    ckpt_read(vol, "v1", 1, buf, sizeof buf);
    printf("checkpoint v1:   \"%s\"\n", buf);

    step("6. Roll the live volume back to \"v1\"");
    must(ckpt_rollback(vol, "v1"), "rollback");
    show();
    printf("-> the live volume points at v1's tree again; the blocks only it used were freed.\n");

    step("7. Delete checkpoint \"v1\"");
    must(ckpt_delete(vol, "v1"), "delete");
    show();
    printf("-> the live volume still uses every block v1 had, so deleting v1 frees nothing:\n"
           "   the root just goes back to 1 reference.\n");

    vol_destroy(vol);
    disk_close(disk);
    return 0;
}
