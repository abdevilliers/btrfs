/*
 * test_ctree.c - the B-tree on its own: random inserts, deletes, item
 * growth and shrinkage in a tree of its own, against a model, until it is
 * several levels tall and then empty again; checked by btrfs check after
 * each commit, and read back after a remount.
 */
#include <errno.h>

#include "ctree.h"
#include "test_util.h"

#define TEST_TREE 200ULL /* an objectid no other tree uses */
#define NKEYS 6000

static uint32_t model_size[NKEYS]; /* 0 = no item */
static uint8_t model_fill[NKEYS];

static BtrfsKey key_of(int i)
{
    return mkkey((uint64_t)(i / 7) * 1000003u, 250, (uint64_t)(i % 7));
}

static Root *test_root(Btrfs *b)
{
    return get_root(b, TEST_TREE);
}

static void verify(Btrfs *b)
{
    Root *r = test_root(b);
    uint8_t buf[4096];
    uint32_t size;
    for (int i = 0; i < NKEYS; i++) {
        BtrfsKey k = key_of(i);
        int ret = btrfs_lookup(b, r, &k, buf, sizeof buf, &size);
        if (!model_size[i]) {
            CHECK(ret == -ENOENT);
            continue;
        }
        CHECK(ret == 0 && size == model_size[i]);
        for (uint32_t j = 0; j < size; j++)
            CHECK(buf[j] == model_fill[i]);
    }
    /* A full walk sees every item once, in order. */
    Path p;
    BtrfsKey k = mkkey(0, 0, 0), prev = { 0, 0, 0 }, f;
    int n = 0, live = 0, ret = btrfs_search_forward(b, r, &k, &p);
    while (ret == 0) {
        item_key(p.nodes[0], p.slots[0], &f);
        CHECK(n == 0 || btrfs_comp_keys(&prev, &f) < 0);
        prev = f;
        n++;
        ret = btrfs_next_item(b, &p);
    }
    for (int i = 0; i < NKEYS; i++)
        live += model_size[i] != 0;
    CHECK(n == live);
}

int main(void)
{
    Disk *d = new_disk(16384);
    Btrfs *b = mount_ok(d);
    Trans t;
    Root *r;
    int maxlevel = 0;

    CHECK(trans_start(b, &t, 1) == 0);
    CHECK(create_empty_tree(&t, TEST_TREE, &r) == 0);
    trans_end(&t);
    commit_and_check(b);

    srand(42);
    for (int round = 0; round < 3; round++) {
        /* Grow, then shrink back to nothing. */
        for (int phase = 0; phase < 2; phase++) {
            for (int step = 0; step < 20000; step++) {
                int i = rand() % NKEYS;
                BtrfsKey k = key_of(i);
                uint8_t data[1500];
                CHECK(trans_start(b, &t, 2) == 0);
                r = test_root(b);
                int grow = phase == 0 ? rand() % 4 != 0 : rand() % 4 == 0;
                if (grow && !model_size[i]) {
                    uint32_t size = (uint32_t)(rand() % 3 == 0 ? rand() % 1400 + 1 : rand() % 60 + 1);
                    memset(data, (uint8_t)step, size);
                    CHECK(btrfs_insert_item(&t, r, &k, data, size) == 0);
                    model_size[i] = size;
                    model_fill[i] = (uint8_t)step;
                } else if (grow && model_size[i] < 1400) {
                    /* Grow the item in place (btrfs_extend_item). */
                    Path p;
                    uint32_t extra = (uint32_t)(rand() % 200 + 1);
                    CHECK(btrfs_search_slot(&t, r, &k, &p, (int)extra, 1) == 0);
                    btrfs_extend_item(&t, &p, extra);
                    memset(item_ptr(p.nodes[0], p.slots[0]), model_fill[i], model_size[i] + extra);
                    model_size[i] += extra;
                } else if (model_size[i]) {
                    if (rand() % 3 == 0 && model_size[i] > 1) {
                        Path p;
                        CHECK(btrfs_search_slot(&t, r, &k, &p, 0, 1) == 0);
                        btrfs_truncate_item(&t, &p, model_size[i] / 2, 1);
                        model_size[i] /= 2;
                    } else {
                        CHECK(btrfs_delete_key(&t, r, &k) == 0);
                        model_size[i] = 0;
                    }
                } else {
                    CHECK(btrfs_delete_key(&t, r, &k) == -ENOENT);
                }
                CHECK(btrfs_insert_item(&t, r, &k, data, 1) == (model_size[i] ? -EEXIST : 0));
                if (!model_size[i])
                    CHECK(btrfs_delete_key(&t, r, &k) == 0);
                trans_end(&t);
                if (eb_level(test_root(b)->node) > maxlevel)
                    maxlevel = eb_level(test_root(b)->node);
                if (step % 2500 == 0) {
                    commit_and_check(b);
                    verify(b);
                }
            }
            if (phase == 1) /* empty it completely */
                for (int i = 0; i < NKEYS; i++)
                    if (model_size[i]) {
                        BtrfsKey k = key_of(i);
                        CHECK(trans_start(b, &t, 1) == 0);
                        CHECK(btrfs_delete_key(&t, test_root(b), &k) == 0);
                        trans_end(&t);
                        model_size[i] = 0;
                    }
            commit_and_check(b);
            verify(b);
        }
        CHECK(eb_level(test_root(b)->node) == 0 && eb_nritems(test_root(b)->node) == 0);
    }
    CHECK(maxlevel >= 2);
    printf("ok   B-tree: inserts, deletes, extend and truncate; grew to level %d and back to one empty leaf\n",
           maxlevel);

    CHECK(btrfs_unmount(b) == 0);
    b = mount_ok(d);
    verify(b);
    CHECK(btrfs_unmount(b) == 0);
    CHECK(btrfs_check(d, 1) == 0);
    disk_close(d);
    printf("ctree: all tests passed\n");
    return 0;
}
