/*
 * free_space.c - free space, in memory and in the free space tree.
 *
 * In memory, each block group keeps a sorted set of free byte ranges (the
 * kernel's free space cache): allocation takes from it, and freed space
 * comes back to it after the commit (pinned until then).
 *
 * On disk, the free space tree (space_cache=v2, fs/btrfs/free-space-tree.c)
 * records the same thing, so a mount need not scan the extent tree:
 *
 *   (bg start, FREE_SPACE_INFO, bg length)   { extent_count, flags }
 *   (start, FREE_SPACE_EXTENT, length)       one per free range
 *
 * It is kept up to date while delayed refs run: inserting an extent item
 * removes its range, deleting one adds it back (merged with neighbours).
 * Bitmaps (used by btrfs for very fragmented block groups) are not modelled.
 */
#include <errno.h>
#include <stdlib.h>

#include "ctree.h"

/* ---------------------------------------------------------- RangeSet */

/* Index of the first range starting after `start`. */
static size_t rs_upper(const RangeSet *s, uint64_t start)
{
    size_t lo = 0, hi = s->n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (s->r[mid].start <= start)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int rs_insert_at(RangeSet *s, size_t i, uint64_t start, uint64_t len)
{
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 16;
        Range *nr = realloc(s->r, nc * sizeof *nr);
        if (!nr)
            return -ENOMEM;
        s->r = nr;
        s->cap = nc;
    }
    memmove(&s->r[i + 1], &s->r[i], (s->n - i) * sizeof *s->r);
    s->r[i].start = start;
    s->r[i].len = len;
    s->n++;
    return 0;
}

static void rs_delete_at(RangeSet *s, size_t i)
{
    memmove(&s->r[i], &s->r[i + 1], (s->n - i - 1) * sizeof *s->r);
    s->n--;
}

int rs_add(RangeSet *s, uint64_t start, uint64_t len)
{
    if (!len)
        return 0;
    size_t i = rs_upper(s, start);
    if (i > 0 && s->r[i - 1].start + s->r[i - 1].len > start)
        return -EINVAL; /* overlaps the range before */
    if (i < s->n && start + len > s->r[i].start)
        return -EINVAL; /* overlaps the range after */
    int merge_prev = i > 0 && s->r[i - 1].start + s->r[i - 1].len == start;
    int merge_next = i < s->n && start + len == s->r[i].start;
    if (merge_prev && merge_next) {
        s->r[i - 1].len += len + s->r[i].len;
        rs_delete_at(s, i);
    } else if (merge_prev) {
        s->r[i - 1].len += len;
    } else if (merge_next) {
        s->r[i].start = start;
        s->r[i].len += len;
    } else {
        return rs_insert_at(s, i, start, len);
    }
    return 0;
}

int rs_remove(RangeSet *s, uint64_t start, uint64_t len)
{
    if (!len)
        return 0;
    size_t i = rs_upper(s, start);
    if (i == 0)
        return -EINVAL;
    Range *r = &s->r[i - 1];
    uint64_t end = start + len, rend = r->start + r->len;
    if (end > rend)
        return -EINVAL; /* not entirely free */
    if (r->start == start && rend == end) {
        rs_delete_at(s, i - 1);
    } else if (r->start == start) {
        r->start = end;
        r->len = rend - end;
    } else if (rend == end) {
        r->len = start - r->start;
    } else {
        r->len = start - r->start;
        return rs_insert_at(s, i, end, rend - end);
    }
    return 0;
}

int rs_contains(const RangeSet *s, uint64_t start, uint64_t len)
{
    size_t i = rs_upper(s, start);
    if (i == 0)
        return 0;
    const Range *r = &s->r[i - 1];
    return start + len <= r->start + r->len;
}

uint64_t rs_total(const RangeSet *s)
{
    uint64_t t = 0;
    for (size_t i = 0; i < s->n; i++)
        t += s->r[i].len;
    return t;
}

void rs_free(RangeSet *s)
{
    free(s->r);
    memset(s, 0, sizeof *s);
}

/* Add [start, start+len) to the in-memory cache, minus superblock copies. */
void cache_add_free(Btrfs *b, BlockGroup *bg, uint64_t start, uint64_t len)
{
    uint64_t disk_bytes = (uint64_t)disk_blocks(b->disk) * SECTORSIZE;
    uint64_t pos = start, end = start + len;
    while (pos < end) {
        uint64_t next = end;
        int excluded = 0;
        for (int m = 1; m < BTRFS_SUPER_MIRROR_MAX; m++) {
            uint64_t s = super_mirror_offset(m), e = s + 64 * 1024;
            if (s + BTRFS_SUPER_INFO_SIZE > disk_bytes)
                continue;
            if (pos >= s && pos < e) {
                excluded = 1;
                next = e < end ? e : end;
                break;
            }
            if (s > pos && s < next)
                next = s;
        }
        if (!excluded)
            rs_add(&bg->free, pos, next - pos);
        pos = next;
    }
}

/* ---------------------------------------------------- free space tree */

static int info_update(Trans *t, BlockGroup *bg, int delta)
{
    BtrfsKey k = mkkey(bg->start, BTRFS_FREE_SPACE_INFO_KEY, bg->len);
    BtrfsFreeSpaceInfo fi;
    if (btrfs_lookup(t->b, t->b->fst_root, &k, &fi, sizeof fi, NULL) < 0)
        return -EIO;
    fi.extent_count = (uint32_t)((int64_t)fi.extent_count + delta);
    return btrfs_update_item(t, t->b->fst_root, &k, &fi, sizeof fi);
}

int fst_add_block_group(Trans *t, BlockGroup *bg)
{
    BtrfsFreeSpaceInfo fi = { 1, 0 };
    BtrfsKey k = mkkey(bg->start, BTRFS_FREE_SPACE_INFO_KEY, bg->len);
    bg->needs_free_space = 0;
    TRY(btrfs_insert_item(t, t->b->fst_root, &k, &fi, sizeof fi));
    k = mkkey(bg->start, BTRFS_FREE_SPACE_EXTENT_KEY, bg->len);
    return btrfs_insert_item(t, t->b->fst_root, &k, NULL, 0);
}

static int ensure_info(Trans *t, BlockGroup *bg)
{
    return bg->needs_free_space ? fst_add_block_group(t, bg) : 0;
}

/* The free extent [start, start+len) is now used. */
int fst_remove(Trans *t, BlockGroup *bg, uint64_t start, uint64_t len)
{
    Btrfs *b = t->b;
    Root *fst = b->fst_root;
    Path p;
    BtrfsKey k = mkkey(start, BTRFS_FREE_SPACE_EXTENT_KEY, (uint64_t)-1), found;

    TRY(ensure_info(t, bg));
    if (btrfs_search_back(b, fst, &k, &p) != 0)
        return -EIO;
    item_key(p.nodes[0], p.slots[0], &found);
    if (found.type != BTRFS_FREE_SPACE_EXTENT_KEY || found.objectid < bg->start ||
        found.objectid > start || found.objectid + found.offset < start + len)
        return -EIO; /* not free in the tree: the trees disagree */

    uint64_t s = found.objectid, e = s + found.offset;
    int pieces = 0;
    TRY(btrfs_delete_key(t, fst, &found));
    if (start > s) {
        k = mkkey(s, BTRFS_FREE_SPACE_EXTENT_KEY, start - s);
        TRY(btrfs_insert_item(t, fst, &k, NULL, 0));
        pieces++;
    }
    if (e > start + len) {
        k = mkkey(start + len, BTRFS_FREE_SPACE_EXTENT_KEY, e - start - len);
        TRY(btrfs_insert_item(t, fst, &k, NULL, 0));
        pieces++;
    }
    return info_update(t, bg, pieces - 1);
}

/* The used extent [start, start+len) is free again. */
int fst_add(Trans *t, BlockGroup *bg, uint64_t start, uint64_t len)
{
    Btrfs *b = t->b;
    Root *fst = b->fst_root;
    Path p;
    BtrfsKey k = mkkey(start, 0, 0), found;
    uint64_t s = start, e = start + len;
    int delta = 1;

    TRY(ensure_info(t, bg));
    /* A free extent ending right here merges with this one. */
    if (btrfs_search_back(b, fst, &k, &p) == 0) {
        item_key(p.nodes[0], p.slots[0], &found);
        if (found.type == BTRFS_FREE_SPACE_EXTENT_KEY && found.objectid >= bg->start &&
            found.objectid + found.offset == start) {
            s = found.objectid;
            TRY(btrfs_delete_key(t, fst, &found));
            delta--;
        }
    }
    /* And one starting right after it. */
    k = mkkey(e, BTRFS_FREE_SPACE_EXTENT_KEY, 0);
    if (e < bg->start + bg->len && btrfs_search_forward(b, fst, &k, &p) == 0) {
        item_key(p.nodes[0], p.slots[0], &found);
        if (found.type == BTRFS_FREE_SPACE_EXTENT_KEY && found.objectid == e) {
            e += found.offset;
            TRY(btrfs_delete_key(t, fst, &found));
            delta--;
        }
    }
    k = mkkey(s, BTRFS_FREE_SPACE_EXTENT_KEY, e - s);
    TRY(btrfs_insert_item(t, fst, &k, NULL, 0));
    return info_update(t, bg, delta);
}

/* load_free_space_tree: fill the in-memory cache of one block group. */
int fst_load_block_group(Btrfs *b, BlockGroup *bg)
{
    Path p;
    BtrfsKey k = mkkey(bg->start, BTRFS_FREE_SPACE_INFO_KEY, bg->len), found;
    BtrfsFreeSpaceInfo fi;
    uint32_t count = 0;

    if (btrfs_lookup(b, b->fst_root, &k, &fi, sizeof fi, NULL) < 0)
        return -EIO;
    if (fi.flags)
        return -EOPNOTSUPP; /* bitmaps */
    k = mkkey(bg->start, BTRFS_FREE_SPACE_EXTENT_KEY, 0);
    int r = btrfs_search_forward(b, b->fst_root, &k, &p);
    while (r == 0) {
        item_key(p.nodes[0], p.slots[0], &found);
        if (found.objectid >= bg->start + bg->len || found.type != BTRFS_FREE_SPACE_EXTENT_KEY)
            break;
        cache_add_free(b, bg, found.objectid, found.offset);
        count++;
        r = btrfs_next_item(b, &p);
    }
    if (r < 0)
        return r;
    return count == fi.extent_count ? 0 : -EIO;
}
