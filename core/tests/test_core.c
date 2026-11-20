/*
 * test_core.c - tests for the shared core: pages, copy-on-write sharing
 * through the root table, and running out of space. Uses no feature.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem_disk.h"
#include "volume.h"

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

static int tree_has(Volume *v, uint32_t root, uint32_t page, const char *want)
{
    char buf[BLOCK_SIZE];
    int r = vol_read_tree(v, root, page, buf, sizeof buf);
    return want ? (r == 0 && strcmp(buf, want) == 0) : r == -ENOENT;
}

static void test_pages(void)
{
    Disk *d = mem_disk_create(64);
    Volume *v = vol_create(d);
    char buf[BLOCK_SIZE];

    CHECK(vol_read(v, 3, buf, sizeof buf) == -ENOENT);
    CHECK(vol_write(v, 3, "hello") == 0);
    CHECK(vol_read(v, 3, buf, sizeof buf) == 0 && strcmp(buf, "hello") == 0);
    CHECK(vol_write(v, 3, "again") == 0);                 /* overwrite */
    CHECK(vol_read(v, 3, buf, sizeof buf) == 0 && strcmp(buf, "again") == 0);
    CHECK(vol_erase(v, 3) == 0);
    CHECK(vol_read(v, 3, buf, sizeof buf) == -ENOENT);
    CHECK(vol_erase(v, 3) == 0);                          /* erase twice: no-op */
    CHECK(vol_write(v, VOL_PAGES, "x") == -EINVAL);
    CHECK(vol_check(v) == 0);

    vol_destroy(v);
    disk_close(d);
    printf("ok   write / read / overwrite / erase\n");
}

static void test_root_table(void)
{
    Disk *d = mem_disk_create(64);
    Volume *v = vol_create(d);
    uint32_t saved;

    CHECK(vol_write(v, 0, "a") == 0);
    uint32_t before = vol_used_blocks(v);

    CHECK(vol_root_add(v, "keep", vol_live_root(v)) == 0);
    CHECK(vol_used_blocks(v) == before);                  /* sharing costs nothing */
    CHECK(vol_root_add(v, "keep", vol_live_root(v)) == -EEXIST);
    CHECK(vol_root_add(v, "", vol_live_root(v)) == -EINVAL);
    CHECK(vol_root_find(v, "keep", &saved) == 0);

    CHECK(vol_write(v, 0, "b") == 0);                     /* copy-on-write */
    CHECK(tree_has(v, vol_live_root(v), 0, "b"));
    CHECK(tree_has(v, saved, 0, "a"));
    CHECK(vol_check(v) == 0);

    vol_set_live_root(v, saved);                          /* live = saved tree */
    CHECK(tree_has(v, vol_live_root(v), 0, "a"));
    CHECK(vol_check(v) == 0);

    CHECK(vol_root_remove(v, "keep") == 0);
    CHECK(vol_root_remove(v, "keep") == -ENOENT);
    CHECK(vol_used_blocks(v) == before);
    CHECK(vol_check(v) == 0);

    vol_destroy(v);
    disk_close(d);
    printf("ok   root table: share, copy-on-write, swap live root, remove\n");
}

static void test_full_disk(void)
{
    Disk *d = mem_disk_create(16 + VOL_META_BLOCKS);
    Volume *v = vol_create(d);
    int r = 0;

    for (uint32_t p = 0; p < VOL_PAGES && r == 0; p++)
        r = vol_write(v, p, "fill");
    CHECK(r == -ENOSPC);                                  /* refused cleanly... */
    CHECK(vol_check(v) == 0);                             /* ...with nothing half-done */

    vol_destroy(v);
    disk_close(d);
    printf("ok   full disk is refused cleanly\n");
}

int main(void)
{
    test_pages();
    test_root_table();
    test_full_disk();
    printf("core: all tests passed\n");
    return 0;
}
