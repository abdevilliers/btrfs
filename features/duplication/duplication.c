#include "duplication.h"
#include "fs.h"
#include <string.h>

int dup_copy_page(Volume *v, uint32_t from, uint32_t to)
{
    if (from >= MAX_PAGES || to >= MAX_PAGES) return -1;
    uint32_t blk = cowtree_get(&v->live, from);     /* 1. find the block   */
    if (blk == 0 && cowtree_get(&v->live, to) == 0) return 0;  /* nothing to do */
    if (blk) alloc_ref(v->alloc, blk);              /* 2. "to" holds a ref */
    cowtree_set(&v->live, to, blk);                 /* 3. point "to" at it */
    return 0;
}

int dup_copy_file(Volume *v, const char *src, const char *dst)
{
    Inode si;
    int s = fs_lookup(&v->live, src, &si);
    if (s < 0 || strcmp(src, dst) == 0) return -1;
    int d = fs_create(v, dst);
    if (d < 0) return -1;
    for (int k = 0; k < PAGES_PER_FILE; k++)
        dup_copy_page(v, fs_data_page(s, k), fs_data_page(d, k));
    fs_set_size(v, d, si.size);
    return 0;
}
