/*
 * test_duplication.c - a duplicate shares the data block, costs no data
 * copy, and the two pages stay independent afterwards.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkpoint.h"
#include "duplication.h"
#include "mem_disk.h"
#include "volume.h"

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

static int page_is(Volume *v, uint32_t page, const char *want)
{
    char buf[BLOCK_SIZE];
    int r = vol_read(v, page, buf, sizeof buf);
    return want ? (r == 0 && strcmp(buf, want) == 0) : r == -ENOENT;
}

static uint32_t refs_of(Volume *v, uint32_t page)
{
    return alloc_refcount(vol_allocator(v), cowtree_get(vol_tree(v), vol_live_root(v), page));
}

static void test_copy(void)
{
    Disk *d = mem_disk_create(64);
    Volume *v = vol_create(d);

    CHECK(vol_write(v, 0, "report") == 0);
    CHECK(vol_write(v, 1, "old") == 0);
    uint32_t used = vol_used_blocks(v);

    CHECK(dup_copy_page(v, 0, 1) == 0);                /* page 1 = duplicate of page 0 */
    CHECK(page_is(v, 1, "report"));
    CHECK(dup_is_shared(v, 0, 1));
    CHECK(refs_of(v, 0) == 2);                         /* one block, two pages */
    CHECK(vol_used_blocks(v) == used - 1);             /* "old" was freed; nothing new used */

    CHECK(dup_copy_page(v, 0, 9) == 0);                /* a third page, in another leaf */
    CHECK(refs_of(v, 0) == 3);
    CHECK(dup_copy_page(v, 0, 9) == 0);                /* again: no change */
    CHECK(refs_of(v, 0) == 3);
    CHECK(dup_copy_page(v, 0, 0) == 0);                /* onto itself: no change */
    CHECK(refs_of(v, 0) == 3);

    CHECK(dup_copy_page(v, 5, 6) == -ENOENT);          /* nothing to duplicate */
    CHECK(dup_copy_page(v, 0, VOL_PAGES) == -EINVAL);
    CHECK(!dup_is_shared(v, 5, 6));                    /* two empty pages share nothing */
    CHECK(vol_check(v) == 0);

    vol_destroy(v);
    disk_close(d);
    printf("ok   duplicate shares the block, copies no data\n");
}

static void test_independent(void)
{
    Disk *d = mem_disk_create(64);
    Volume *v = vol_create(d);

    CHECK(vol_write(v, 0, "original") == 0);
    CHECK(dup_copy_page(v, 0, 1) == 0);

    CHECK(vol_write(v, 1, "changed copy") == 0);       /* write the duplicate... */
    CHECK(page_is(v, 0, "original"));                  /* ...the source is untouched */
    CHECK(page_is(v, 1, "changed copy"));
    CHECK(!dup_is_shared(v, 0, 1));
    CHECK(refs_of(v, 0) == 1);

    CHECK(dup_copy_page(v, 0, 2) == 0);
    CHECK(vol_erase(v, 0) == 0);                       /* erase the source... */
    CHECK(page_is(v, 2, "original"));                  /* ...the duplicate keeps the data */
    CHECK(refs_of(v, 2) == 1);
    CHECK(vol_check(v) == 0);

    vol_destroy(v);
    disk_close(d);
    printf("ok   source and duplicate are independent after a write or erase\n");
}

static void test_with_checkpoint(void)
{
    Disk *d = mem_disk_create(64);
    Volume *v = vol_create(d);
    char buf[BLOCK_SIZE];

    CHECK(vol_write(v, 0, "a") == 0);
    CHECK(vol_write(v, 1, "b") == 0);
    CHECK(ckpt_create(v, "before") == 0);

    CHECK(dup_copy_page(v, 0, 1) == 0);                /* the path is shared: it is copied first */
    CHECK(page_is(v, 1, "a"));
    CHECK(ckpt_read(v, "before", 1, buf, sizeof buf) == 0 && strcmp(buf, "b") == 0);
    CHECK(vol_check(v) == 0);

    CHECK(ckpt_rollback(v, "before") == 0);            /* the duplication is undone */
    CHECK(page_is(v, 1, "b"));
    CHECK(!dup_is_shared(v, 0, 1));
    CHECK(vol_check(v) == 0);

    vol_destroy(v);
    disk_close(d);
    printf("ok   works together with checkpoints\n");
}

/* Random writes, erases and duplications, compared with a plain model. */
static void test_random(void)
{
    Disk *d = mem_disk_create(256);
    Volume *v = vol_create(d);
    static char model[VOL_PAGES][BLOCK_SIZE]; /* "" = empty page */

    srand(11);
    for (int step = 0; step < 5000; step++) {
        uint32_t a = (uint32_t)(rand() % VOL_PAGES), b = (uint32_t)(rand() % VOL_PAGES);
        int op = rand() % 4;

        if (op == 0) {
            char text[BLOCK_SIZE];
            snprintf(text, sizeof text, "step %d", step);
            CHECK(vol_write(v, a, text) == 0);
            strcpy(model[a], text);
        } else if (op == 1) {
            CHECK(vol_erase(v, a) == 0);
            model[a][0] = '\0';
        } else {
            int r = dup_copy_page(v, a, b);
            CHECK(r == (model[a][0] ? 0 : -ENOENT));
            if (r == 0)
                memmove(model[b], model[a], sizeof model[b]);
        }
        for (uint32_t p = 0; p < VOL_PAGES; p++)
            CHECK(page_is(v, p, model[p][0] ? model[p] : NULL));
        CHECK(vol_check(v) == 0);
    }
    vol_destroy(v);
    disk_close(d);
    printf("ok   5000 random writes, erases and duplications\n");
}

int main(void)
{
    test_copy();
    test_independent();
    test_with_checkpoint();
    test_random();
    printf("duplication: all tests passed\n");
    return 0;
}
