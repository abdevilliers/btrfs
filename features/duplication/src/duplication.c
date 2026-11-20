/*
 * duplication.c - duplicate a page by sharing its data block.
 */
#include <errno.h>

#include "duplication.h"

static uint32_t block_of(Volume *v, uint32_t page)
{
    return cowtree_get(vol_tree(v), vol_live_root(v), page);
}

int dup_copy_page(Volume *v, uint32_t from, uint32_t to)
{
    if (from >= VOL_PAGES || to >= VOL_PAGES)
        return -EINVAL;

    /* 1. Find the data block of the source page. */
    uint32_t blk = block_of(v, from);
    if (!blk)
        return -ENOENT;
    if (from == to)
        return 0;

    /* 2. One more page will point at it: reference count + 1. */
    alloc_ref(vol_allocator(v), blk);

    /* 3. Point the target page at it. The tree takes over the reference
     *    from step 2, drops the target's old block, and copies the path
     *    first if a checkpoint shares it. */
    int r = vol_map_page(v, to, blk);
    if (r)
        alloc_unref(vol_allocator(v), blk); /* failed: give the reference back */
    return r;
}

int dup_is_shared(Volume *v, uint32_t a, uint32_t b)
{
    uint32_t blk = block_of(v, a);
    return blk != 0 && blk == block_of(v, b);
}
