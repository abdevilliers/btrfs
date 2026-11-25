/*
 * duplication.c - reflink copies, on top of the core's fs_clone_range
 * (btrfs_clone in fs/btrfs/reflink.c).
 */
#include <errno.h>

#include "duplication.h"

int dup_copy_file(Fs *fs, const char *src, const char *dst)
{
    FsStat st;
    int r = fs_stat(fs, src, &st);
    if (r < 0)
        return r;
    if (st.type != FS_FILE)
        return -EISDIR;
    if ((r = fs_create(fs, dst)) < 0)
        return r;
    if (st.size && (r = fs_clone_range(fs, src, 0, dst, 0, 0)) < 0)
        fs_unlink(fs, dst); /* leave no half-made copy */
    return r;
}

int dup_copy_range(Fs *fs, const char *src, uint64_t src_off, const char *dst, uint64_t dst_off,
                   uint64_t len)
{
    return fs_clone_range(fs, src, src_off, dst, dst_off, len);
}

typedef struct {
    uint64_t off, phys;
} Find;

static void find_phys(void *arg, const FsExtent *e)
{
    Find *f = arg;
    if (!(e->flags & FIEMAP_EXTENT_INLINE) && f->off >= e->logical && f->off < e->logical + e->length)
        f->phys = e->physical + (f->off - e->logical);
}

int dup_is_shared(Fs *fs, const char *a, uint64_t off_a, const char *b, uint64_t off_b)
{
    Find fa = { off_a, 0 }, fb = { off_b, 0 };
    if (fs_fiemap(fs, a, find_phys, &fa) < 0 || fs_fiemap(fs, b, find_phys, &fb) < 0)
        return 0;
    return fa.phys != 0 && fa.phys == fb.phys;
}
