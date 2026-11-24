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

int dup_copy_file(Fs *fs, const char *src, const char *dst)
{
    Volume *v = fs_volume(fs);
    FsStat st;
    int r;

    /* 1. The source must be a file. */
    if ((r = fs_stat(fs, src, &st)) < 0)
        return r;
    if (st.type != FS_FILE)
        return -EISDIR;

    /* 2. A new, empty file: its own inode from the free list. */
    if ((r = fs_create(fs, dst)) < 0)
        return r;

    /* 3. For each page of the source, give the copy a page of its own in
     *    the filesystem (bmap with alloc) and point it at the source's
     *    data block (dup_copy_page: reference count + 1). */
    for (uint32_t i = 0; i * BLOCK_SIZE < st.size && r >= 0; i++) {
        uint32_t from, to;
        r = fs_bmap(fs, src, i, 0, &from);
        if (r < 0 || from == 0)
            continue; /* a gap in the source stays a gap */
        r = fs_bmap(fs, dst, i, 1, &to);
        if (r >= 0)
            r = dup_copy_page(v, from, to);
        if (r == -ENOENT)
            r = 0; /* the source page holds no block yet: it reads as zeros */
    }

    /* 4. Same size as the source. */
    if (r >= 0)
        r = fs_truncate(fs, dst, st.size);
    if (r < 0)
        fs_unlink(fs, dst); /* undo: leave no half-made copy */
    return r < 0 ? r : 0;
}
