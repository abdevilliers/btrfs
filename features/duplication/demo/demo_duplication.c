/*
 * demo_duplication.c - duplicate a page, then change the duplicate,
 * printing the tree and every reference count after each step.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "duplication.h"
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
    disk = mem_disk_create(64);
    vol = vol_create(disk);
    if (!disk || !vol)
        return 1;

    step("1. Write page 0");
    must(vol_write(vol, 0, "quarterly report"), "write");
    show();

    step("2. Duplicate page 0 into page 1 and page 5");
    must(dup_copy_page(vol, 0, 1), "duplicate");
    must(dup_copy_page(vol, 0, 5), "duplicate");
    show();
    printf("-> three pages, ONE data block with 3 references. The text was never copied;\n"
           "   the only writes are the tree nodes that now point at it.\n");

    step("3. Change the duplicate in page 1");
    must(vol_write(vol, 1, "quarterly report, edited"), "write");
    show();
    printf("-> page 1 got its own new block. Pages 0 and 5 still share the original,\n"
           "   which is down to 2 references.\n");

    step("4. Erase the original, page 0");
    must(vol_erase(vol, 0), "erase");
    show();
    printf("-> the data is still there for page 5: a block is freed only when its\n"
           "   last reference goes.\n");

    vol_destroy(vol);
    disk_close(disk);
    return 0;
}
