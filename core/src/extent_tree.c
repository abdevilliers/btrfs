/*
 * extent_tree.c - who uses which bytes (fs/btrfs/extent-tree.c,
 * block-group.c, space-info.c).
 *
 * Every allocated extent has an item in the extent tree:
 *
 *   tree block:  (bytenr, METADATA_ITEM, level)   { refs, generation, flags }
 *   data:        (bytenr, EXTENT_ITEM, length)    { refs, generation, flags }
 *
 * and one back-reference item per user, saying WHO holds each reference:
 *
 *   (bytenr, TREE_BLOCK_REF, root)        a block of tree `root` points here
 *   (bytenr, SHARED_BLOCK_REF, parent)    tree block `parent` points here
 *   (bytenr, EXTENT_DATA_REF, hash)       { root, inode, offset, count }
 *   (bytenr, SHARED_DATA_REF, parent)     { count }: leaf `parent` points here
 *
 * The plain refs are "implicit": they name the tree that owns the parent.
 * The SHARED refs name the parent block itself; a block whose children are
 * referenced that way has FULL_BACKREF in its flags. Free space is
 * whatever no extent item covers, grouped into block groups (DATA or
 * METADATA), each recorded by a BLOCK_GROUP_ITEM.
 */
#include <errno.h>
#include <stdlib.h>

#include "ctree.h"

#define MiB (1024ULL * 1024)
#define STRIPE_LEN (64ULL * 1024)

/* ------------------------------------------------------- superblocks */

uint64_t super_mirror_offset(int mirror)
{
    return mirror ? (16ULL * 1024) << (12 * mirror) : BTRFS_SUPER_INFO_OFFSET;
}

int is_super_range(uint64_t start, uint64_t len, uint64_t disk_bytes)
{
    for (int m = 0; m < BTRFS_SUPER_MIRROR_MAX; m++) {
        uint64_t s = super_mirror_offset(m);
        if (s + BTRFS_SUPER_INFO_SIZE > disk_bytes)
            continue;
        if (start < s + STRIPE_LEN && s < start + len)
            return 1;
    }
    return 0;
}

/* ----------------------------------------------------- block groups */

static int space_of(uint64_t bg_flags)
{
    return bg_flags & BTRFS_BLOCK_GROUP_DATA ? SPACE_DATA : SPACE_METADATA;
}

BlockGroup *find_block_group(Btrfs *b, uint64_t bytenr)
{
    size_t lo = 0, hi = b->nbgs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        BlockGroup *bg = b->bgs[mid];
        if (bytenr < bg->start)
            hi = mid;
        else if (bytenr >= bg->start + bg->len)
            lo = mid + 1;
        else
            return bg;
    }
    return NULL;
}

static uint64_t super_bytes_in(Btrfs *b, uint64_t start, uint64_t len)
{
    uint64_t disk_bytes = (uint64_t)disk_blocks(b->disk) * SECTORSIZE, n = 0;
    for (int m = 1; m < BTRFS_SUPER_MIRROR_MAX; m++) {
        uint64_t s = super_mirror_offset(m), e = s + STRIPE_LEN;
        if (s + BTRFS_SUPER_INFO_SIZE > disk_bytes)
            continue;
        uint64_t a = s > start ? s : start, z = e < start + len ? e : start + len;
        if (a < z)
            n += z - a;
    }
    return n;
}

static BlockGroup *add_block_group(Btrfs *b, uint64_t start, uint64_t len, uint64_t flags)
{
    BlockGroup *bg = calloc(1, sizeof *bg);
    BlockGroup **nb = realloc(b->bgs, (b->nbgs + 1) * sizeof *nb);
    if (!bg || !nb) {
        free(bg);
        if (nb)
            b->bgs = nb;
        return NULL;
    }
    b->bgs = nb;
    bg->start = start;
    bg->len = len;
    bg->flags = flags;
    bg->bytes_super = super_bytes_in(b, start, len);
    size_t i = b->nbgs;
    while (i > 0 && b->bgs[i - 1]->start > start) {
        b->bgs[i] = b->bgs[i - 1];
        i--;
    }
    b->bgs[i] = bg;
    b->nbgs++;
    SpaceInfo *s = &b->space[space_of(flags)];
    s->total += len;
    s->readonly += bg->bytes_super;
    return bg;
}

/* btrfs_chunk_alloc: turn unallocated disk space into a new block group.
 * Its size is 10% of the disk (at least 1 MiB; at most 1 GiB for data,
 * 256 MiB for metadata), or whatever is left. */
int alloc_chunk(Btrfs *b, uint64_t type)
{
    if (b->read_only)
        return -EROFS;
    uint64_t disk_bytes = (uint64_t)disk_blocks(b->disk) * SECTORSIZE;
    uint64_t target = disk_bytes / 10 / MiB * MiB;
    uint64_t cap = type & BTRFS_BLOCK_GROUP_DATA ? 1024 * MiB : 256 * MiB;
    if (target < MiB)
        target = MiB;
    if (target > cap)
        target = cap;

    uint64_t cursor = MiB; /* the first MiB is never allocated */
    for (size_t i = 0; i <= b->nbgs; i++) {
        uint64_t gap_end = i < b->nbgs ? b->bgs[i]->start : disk_bytes / STRIPE_LEN * STRIPE_LEN;
        if (gap_end > cursor && gap_end - cursor >= 4 * STRIPE_LEN) {
            uint64_t len = gap_end - cursor;
            if (len > target)
                len = target;
            len = len / STRIPE_LEN * STRIPE_LEN;
            BlockGroup *bg = add_block_group(b, cursor, len, type);
            if (!bg)
                return -ENOMEM;
            bg->pending = 1;
            bg->needs_free_space = 1;
            cache_add_free(b, bg, cursor, len);
            b->trans_dirty = 1;
            return 0;
        }
        if (i < b->nbgs)
            cursor = b->bgs[i]->start + b->bgs[i]->len;
    }
    return -ENOSPC;
}

/* Mount: read every BLOCK_GROUP_ITEM, and each one's free space. */
int load_block_groups(Btrfs *b)
{
    Path p;
    BtrfsKey k = mkkey(0, 0, 0), found;
    int r = btrfs_search_forward(b, b->extent_root, &k, &p);
    while (r == 0) {
        item_key(p.nodes[0], p.slots[0], &found);
        if (found.type == BTRFS_BLOCK_GROUP_ITEM_KEY) {
            BtrfsBlockGroupItem bi;
            memcpy(&bi, item_ptr(p.nodes[0], p.slots[0]), sizeof bi);
            BlockGroup *bg = add_block_group(b, found.objectid, found.offset, bi.flags);
            if (!bg)
                return -ENOMEM;
            bg->used = bi.used;
            b->space[space_of(bi.flags)].used += bi.used;
            TRY(fst_load_block_group(b, bg));
            if (rs_total(&bg->free) != bg->len - bg->used - bg->bytes_super)
                return -EIO; /* the free space tree disagrees with the block group */
        }
        r = btrfs_next_item(b, &p);
    }
    return r < 0 ? r : 0;
}

int create_pending_block_groups(Trans *t)
{
    Btrfs *b = t->b;
    for (size_t i = 0; i < b->nbgs; i++) {
        BlockGroup *bg = b->bgs[i];
        if (!bg->pending)
            continue;
        BtrfsBlockGroupItem bi = { bg->used, BTRFS_FIRST_FREE_OBJECTID, bg->flags };
        BtrfsKey k = mkkey(bg->start, BTRFS_BLOCK_GROUP_ITEM_KEY, bg->len);
        bg->pending = 0;
        bg->dirty = 0;
        TRY(btrfs_insert_item(t, b->extent_root, &k, &bi, sizeof bi));
        if (bg->needs_free_space)
            TRY(fst_add_block_group(t, bg));
        i = (size_t)-1; /* inserting may have made another one: start over */
    }
    return 0;
}

int write_dirty_block_groups(Trans *t)
{
    Btrfs *b = t->b;
    for (size_t i = 0; i < b->nbgs; i++) {
        BlockGroup *bg = b->bgs[i];
        if (!bg->dirty || bg->pending)
            continue;
        bg->dirty = 0;
        BtrfsBlockGroupItem bi = { bg->used, BTRFS_FIRST_FREE_OBJECTID, bg->flags };
        BtrfsKey k = mkkey(bg->start, BTRFS_BLOCK_GROUP_ITEM_KEY, bg->len);
        TRY(btrfs_update_item(t, b->extent_root, &k, &bi, sizeof bi));
    }
    return 0;
}

int any_dirty_block_groups(const Btrfs *b)
{
    for (size_t i = 0; i < b->nbgs; i++)
        if (b->bgs[i]->dirty || b->bgs[i]->pending)
            return 1;
    return 0;
}

uint64_t total_bytes_used(const Btrfs *b)
{
    uint64_t n = 0;
    for (size_t i = 0; i < b->nbgs; i++)
        n += b->bgs[i]->used;
    return n;
}

void block_groups_destroy(Btrfs *b)
{
    for (size_t i = 0; i < b->nbgs; i++) {
        rs_free(&b->bgs[i]->free);
        free(b->bgs[i]);
    }
    free(b->bgs);
    b->bgs = NULL;
    b->nbgs = 0;
    rs_free(&b->pinned);
}

/* ------------------------------------------------------------ pinning */

static void pin_extent(Btrfs *b, BlockGroup *bg, uint64_t start, uint64_t len)
{
    rs_add(&b->pinned, start, len);
    bg->pinned += len;
    b->space[space_of(bg->flags)].pinned += len;
}

/* After the commit: space freed in it can be used again. */
void unpin_extents(Btrfs *b)
{
    for (size_t i = 0; i < b->pinned.n; i++) {
        uint64_t pos = b->pinned.r[i].start, end = pos + b->pinned.r[i].len;
        while (pos < end) {
            BlockGroup *bg = find_block_group(b, pos);
            if (!bg)
                break;
            uint64_t e = bg->start + bg->len < end ? bg->start + bg->len : end;
            bg->pinned -= e - pos;
            b->space[space_of(bg->flags)].pinned -= e - pos;
            cache_add_free(b, bg, pos, e - pos);
            pos = e;
        }
    }
    rs_free(&b->pinned);
}

/* -------------------------------------------------------------- space */

static uint64_t space_free(const SpaceInfo *s)
{
    uint64_t busy = s->used + s->reserved + s->pinned + s->readonly + s->may_use;
    return s->total > busy ? s->total - busy : 0;
}

/* btrfs_calc_insert_metadata_size: enough to COW and split a path of the
 * deepest possible tree, per item. */
uint64_t metadata_bytes_for(const Btrfs *b, unsigned items)
{
    return (uint64_t)items * b->nodesize * BTRFS_MAX_LEVEL * 2;
}

int reserve_metadata(Btrfs *b, uint64_t bytes, int flush)
{
    for (;;) {
        SpaceInfo *s = &b->space[SPACE_METADATA];
        if (space_free(s) >= bytes) {
            s->may_use += bytes;
            return 0;
        }
        if (alloc_chunk(b, BTRFS_BLOCK_GROUP_METADATA) == 0)
            continue;
        /* Flushing: committing turns pinned space into free space. */
        if (flush && !b->in_commit && !b->no_flush && b->trans_dirty) {
            flush = 0;
            TRY(btrfs_commit(b));
            continue;
        }
        return -ENOSPC;
    }
}

void release_metadata(Btrfs *b, uint64_t bytes)
{
    b->space[SPACE_METADATA].may_use -= bytes;
}

/* The global block reserve: space kept for what a commit itself writes
 * (the extent tree, free space tree and root tree). */
void refill_global_rsv(Btrfs *b)
{
    SpaceInfo *s = &b->space[SPACE_METADATA];
    uint64_t trees = 0;
    Root *cow_only[] = { b->tree_root, b->extent_root, b->csum_root, b->fst_root, b->uuid_root };
    for (size_t i = 0; i < sizeof cow_only / sizeof *cow_only; i++)
        if (cow_only[i])
            trees += cow_only[i]->item.bytes_used;
    uint64_t target = 2 * trees + 16ULL * b->nodesize;

    if (b->global_rsv_left > target) {
        s->may_use -= b->global_rsv_left - target;
        b->global_rsv_left = target;
    } else {
        uint64_t want = target - b->global_rsv_left;
        if (space_free(s) < want && alloc_chunk(b, BTRFS_BLOCK_GROUP_METADATA) == 0) {
        }
        uint64_t got = space_free(s) < want ? space_free(s) : want;
        s->may_use += got;
        b->global_rsv_left += got;
    }
    b->global_rsv_size = target;
}

int reserve_data_space(Btrfs *b, uint64_t bytes)
{
    int flush = 1;
    for (;;) {
        SpaceInfo *s = &b->space[SPACE_DATA];
        if (space_free(s) >= bytes) {
            s->may_use += bytes;
            return 0;
        }
        if (alloc_chunk(b, BTRFS_BLOCK_GROUP_DATA) == 0)
            continue;
        if (flush && !b->in_commit && !b->no_flush && b->trans_dirty) {
            flush = 0;
            TRY(btrfs_commit(b));
            continue;
        }
        return -ENOSPC;
    }
}

void release_data_space(Btrfs *b, uint64_t bytes)
{
    b->space[SPACE_DATA].may_use -= bytes;
}

/* ---------------------------------------------------------- allocation */

/* First fit in a block group; tree blocks never cross a 64 KiB stripe. */
static int find_in_bg(BlockGroup *bg, uint64_t len, int meta, uint64_t *out)
{
    for (size_t i = 0; i < bg->free.n; i++) {
        uint64_t s = bg->free.r[i].start, e = s + bg->free.r[i].len;
        if (meta && s / STRIPE_LEN != (s + len - 1) / STRIPE_LEN)
            s = (s + STRIPE_LEN - 1) / STRIPE_LEN * STRIPE_LEN;
        if (s + len <= e) {
            *out = s;
            return 0;
        }
    }
    return -ENOSPC;
}

static int take_metadata_budget(Trans *t)
{
    Btrfs *b = t->b;
    SpaceInfo *s = &b->space[SPACE_METADATA];
    uint64_t ns = b->nodesize;

    if (t->rsv >= ns) {
        t->rsv -= ns;
        s->may_use -= ns;
        return 0;
    }
    if (b->global_rsv_left >= ns) {
        b->global_rsv_left -= ns;
        s->may_use -= ns;
        return 0;
    }
    if (space_free(s) >= ns)
        return 0;
    if (alloc_chunk(b, BTRFS_BLOCK_GROUP_METADATA) == 0 && space_free(s) >= ns)
        return 0;
    return -ENOSPC;
}

static int find_metadata_block(Btrfs *b, BlockGroup **bgp, uint64_t *start)
{
    for (int tries = 0; tries < 2; tries++) {
        for (size_t i = 0; i < b->nbgs; i++) {
            BlockGroup *bg = b->bgs[i];
            if (!(bg->flags & BTRFS_BLOCK_GROUP_METADATA))
                continue;
            if (find_in_bg(bg, b->nodesize, 1, start) == 0) {
                *bgp = bg;
                return 0;
            }
        }
        if (alloc_chunk(b, BTRFS_BLOCK_GROUP_METADATA) < 0)
            break;
    }
    return -ENOSPC;
}

/* btrfs_alloc_tree_block: a new, empty block of `root` at `level`. Its
 * reference (TREE_BLOCK_REF from root) is queued as a delayed ref; the
 * extent item is inserted when that runs. */
int alloc_tree_block(Trans *t, Root *root, int level, ExtentBuffer **out)
{
    Btrfs *b = t->b;
    BlockGroup *bg;
    uint64_t start, ns = b->nodesize;

    TRY(take_metadata_budget(t));
    TRY(find_metadata_block(b, &bg, &start));
    TRY(rs_remove(&bg->free, start, ns));
    bg->reserved += ns;
    b->space[SPACE_METADATA].reserved += ns;

    ExtentBuffer *eb = eb_new(b, start);
    if (!eb)
        return -ENOMEM;
    memcpy(eb->data + HDR_FSID, b->fsid, BTRFS_FSID_SIZE);
    memcpy(eb->data + HDR_CHUNK_UUID, b->fsid, BTRFS_UUID_SIZE);
    eb_set_bytenr(eb, start);
    eb_set_flags(eb, BTRFS_MIXED_BACKREF_REV << BTRFS_BACKREF_REV_SHIFT);
    eb_set_gen(eb, b->transid);
    eb_set_owner(eb, root->objectid);
    eb_set_level(eb, level);
    btrfs_mark_dirty(t, eb);
    root->item.bytes_used += ns;

    DelayedRef ref = { BTRFS_TREE_BLOCK_REF_KEY, root->objectid, 0, 0, 0, 1 };
    TRY(add_delayed_ref(b, start, ns, 0, level, &ref, 1));
    *out = eb;
    return 0;
}

static Root *loaded_root(Btrfs *b, uint64_t id)
{
    for (Root *r = b->roots; r; r = r->next)
        if (r->objectid == id)
            return r;
    return NULL;
}

/* btrfs_free_tree_block: `root_id`'s reference to `eb` goes. If the block
 * was made in this transaction and never written, its space is free at
 * once (check_ref_cleanup); otherwise the delayed ref frees it at commit. */
int free_tree_block(Trans *t, uint64_t root_id, ExtentBuffer *eb, uint64_t parent, int last_ref)
{
    Btrfs *b = t->b;
    uint64_t ns = b->nodesize;
    DelayedRef ref = { parent ? BTRFS_SHARED_BLOCK_REF_KEY : BTRFS_TREE_BLOCK_REF_KEY,
                       parent ? 0 : root_id, parent, 0, 0, -1 };

    if (last_ref) {
        Root *owner = loaded_root(b, eb_owner(eb));
        if (owner && owner->item.bytes_used >= ns)
            owner->item.bytes_used -= ns;
    }
    if (last_ref && eb_gen(eb) == b->transid && !(eb_flags(eb) & BTRFS_HEADER_FLAG_WRITTEN)) {
        DelayedHead *h = find_delayed_head(b, eb->start);
        if (h && h->must_insert && h->nrefs == 1 && !h->set_flags && h->refs[0].count == 1 &&
            h->refs[0].type == ref.type && h->refs[0].root == ref.root &&
            h->refs[0].parent == ref.parent) {
            BlockGroup *bg = find_block_group(b, eb->start);
            remove_delayed_head(b, h);
            free_delayed_head(h);
            bg->reserved -= ns;
            b->space[SPACE_METADATA].reserved -= ns;
            TRY(rs_add(&bg->free, eb->start, ns));
            eb_forget(b, eb);
            return 0;
        }
    }
    return add_delayed_ref(b, eb->start, ns, 0, eb_level(eb), &ref, 0);
}

int drop_tree_ref(Trans *t, uint64_t bytenr, int level, uint64_t root_id, uint64_t parent)
{
    DelayedRef ref = { parent ? BTRFS_SHARED_BLOCK_REF_KEY : BTRFS_TREE_BLOCK_REF_KEY,
                       parent ? 0 : root_id, parent, 0, 0, -1 };
    return add_delayed_ref(t->b, bytenr, t->b->nodesize, 0, level, &ref, 0);
}

int add_data_ref(Trans *t, uint64_t bytenr, uint64_t num_bytes, uint64_t root, uint64_t ino,
                 uint64_t offset, int64_t delta, int must_insert)
{
    DelayedRef ref = { BTRFS_EXTENT_DATA_REF_KEY, root, 0, ino, offset, delta };
    return add_delayed_ref(t->b, bytenr, num_bytes, 1, 0, &ref, must_insert);
}

int alloc_data_extent(Btrfs *b, uint64_t want, uint64_t *start, uint64_t *len)
{
    BlockGroup *best_bg = NULL;
    uint64_t best_start = 0, best_len = 0;

    for (size_t i = 0; i < b->nbgs; i++) {
        BlockGroup *bg = b->bgs[i];
        if (!(bg->flags & BTRFS_BLOCK_GROUP_DATA))
            continue;
        for (size_t j = 0; j < bg->free.n; j++) {
            uint64_t l = bg->free.r[j].len;
            if (l >= want) {
                best_bg = bg;
                best_start = bg->free.r[j].start;
                best_len = want;
                goto found;
            }
            if (l > best_len) {
                best_bg = bg;
                best_start = bg->free.r[j].start;
                best_len = l;
            }
        }
    }
    if (!best_bg)
        return -ENOSPC;
found:
    TRY(rs_remove(&best_bg->free, best_start, best_len));
    best_bg->reserved += best_len;
    SpaceInfo *s = &b->space[SPACE_DATA];
    s->reserved += best_len;
    s->may_use -= best_len < s->may_use ? best_len : s->may_use;
    *start = best_start;
    *len = best_len;
    return 0;
}

void unalloc_data_extent(Btrfs *b, uint64_t start, uint64_t len)
{
    BlockGroup *bg = find_block_group(b, start);
    if (!bg)
        return;
    rs_add(&bg->free, start, len);
    bg->reserved -= len;
    b->space[SPACE_DATA].reserved -= len;
    b->space[SPACE_DATA].may_use += len;
}

/* --------------------------------------------- reference counts and refs */

static int find_extent_item(Btrfs *b, uint64_t bytenr, int level, int is_data, uint64_t num_bytes,
                            BtrfsKey *key, BtrfsExtentItem *ei)
{
    if (is_data && !num_bytes) {
        Path p;
        BtrfsKey k = mkkey(bytenr, BTRFS_EXTENT_ITEM_KEY, 0), f;
        if (btrfs_search_forward(b, b->extent_root, &k, &p) != 0)
            return -ENOENT;
        item_key(p.nodes[0], p.slots[0], &f);
        if (f.objectid != bytenr || f.type != BTRFS_EXTENT_ITEM_KEY)
            return -ENOENT;
        *key = f;
        memcpy(ei, item_ptr(p.nodes[0], p.slots[0]), sizeof *ei);
        return 0;
    }
    *key = is_data ? mkkey(bytenr, BTRFS_EXTENT_ITEM_KEY, num_bytes)
                   : mkkey(bytenr, BTRFS_METADATA_ITEM_KEY, (uint64_t)level);
    return btrfs_lookup(b, b->extent_root, key, ei, sizeof *ei, NULL);
}

/* btrfs_lookup_extent_info: the reference count and flags, including the
 * changes still queued as delayed refs. */
int lookup_extent_info(Btrfs *b, uint64_t bytenr, int level, int is_data, uint64_t *refs,
                       uint64_t *flags)
{
    BtrfsKey k;
    BtrfsExtentItem ei;
    int64_t n = 0;
    uint64_t f = 0;

    int r = find_extent_item(b, bytenr, level, is_data, 0, &k, &ei);
    if (r == 0) {
        n = (int64_t)ei.refs;
        f = ei.flags;
    } else if (r != -ENOENT) {
        return r;
    }
    DelayedHead *h = find_delayed_head(b, bytenr);
    if (h) {
        n += h->ref_mod;
        f |= h->set_flags;
        if (h->must_insert && r == -ENOENT)
            f |= is_data ? BTRFS_EXTENT_FLAG_DATA : BTRFS_EXTENT_FLAG_TREE_BLOCK;
    }
    *refs = n > 0 ? (uint64_t)n : 0;
    *flags = f;
    return 0;
}

/* __btrfs_mod_ref: one reference for every child of `eb` (or every data
 * extent its file extent items use), named by the owner of `eb` or, with
 * full_backref, by `eb` itself. */
static int mod_ref(Trans *t, ExtentBuffer *eb, int full, int64_t delta)
{
    Btrfs *b = t->b;
    int level = eb_level(eb);
    uint64_t owner = eb_owner(eb), parent = full ? eb->start : 0;

    for (int i = 0; i < (int)eb_nritems(eb); i++) {
        if (level == 0) {
            BtrfsKey k;
            item_key(eb, i, &k);
            if (k.type != BTRFS_EXTENT_DATA_KEY)
                continue;
            BtrfsFileExtentItem fi;
            memcpy(&fi, item_ptr(eb, i), sizeof fi < item_size(eb, i) ? sizeof fi : item_size(eb, i));
            if (fi.type == BTRFS_FILE_EXTENT_INLINE || fi.disk_bytenr == 0)
                continue;
            DelayedRef ref;
            if (full)
                ref = (DelayedRef){ BTRFS_SHARED_DATA_REF_KEY, 0, parent, 0, 0, delta };
            else
                ref = (DelayedRef){ BTRFS_EXTENT_DATA_REF_KEY, owner, 0, k.objectid,
                                    k.offset - fi.offset, delta };
            TRY(add_delayed_ref(b, fi.disk_bytenr, fi.disk_num_bytes, 1, 0, &ref, 0));
        } else {
            DelayedRef ref = { full ? BTRFS_SHARED_BLOCK_REF_KEY : BTRFS_TREE_BLOCK_REF_KEY,
                               full ? 0 : owner, parent, 0, 0, delta };
            TRY(add_delayed_ref(b, node_blockptr(eb, i), b->nodesize, 0, level - 1, &ref, 0));
        }
    }
    return 0;
}

int btrfs_inc_ref(Trans *t, Root *root, ExtentBuffer *eb, int full_backref)
{
    (void)root;
    return mod_ref(t, eb, full_backref, 1);
}

int btrfs_dec_ref(Trans *t, Root *root, ExtentBuffer *eb, int full_backref)
{
    (void)root;
    return mod_ref(t, eb, full_backref, -1);
}

int set_extent_flags(Trans *t, ExtentBuffer *eb, uint64_t flags)
{
    return add_delayed_extent_op(t->b, eb->start, t->b->nodesize, 0, eb_level(eb), flags);
}

/* ------------------------------------------------- running a delayed head */

static int find_data_ref(Btrfs *b, uint64_t bytenr, const DelayedRef *r, BtrfsKey *key,
                         BtrfsExtentDataRef *dr)
{
    Path p;
    BtrfsKey k = mkkey(bytenr, BTRFS_EXTENT_DATA_REF_KEY, hash_extent_data_ref(r->root, r->owner, r->offset));
    int ret = btrfs_search_forward(b, b->extent_root, &k, &p);
    while (ret == 0) {
        BtrfsKey f;
        item_key(p.nodes[0], p.slots[0], &f);
        if (f.objectid != bytenr || f.type != BTRFS_EXTENT_DATA_REF_KEY)
            break;
        memcpy(dr, item_ptr(p.nodes[0], p.slots[0]), sizeof *dr);
        if (dr->root == r->root && dr->objectid == r->owner && dr->offset == r->offset) {
            *key = f;
            return 0;
        }
        ret = btrfs_next_item(b, &p);
    }
    return ret < 0 ? ret : -ENOENT;
}

/* Add or drop `count` of one back-reference item. */
static int backref_mod(Trans *t, uint64_t bytenr, const DelayedRef *r, int64_t count)
{
    Btrfs *b = t->b;
    Root *ext = b->extent_root;
    BtrfsKey k;

    switch (r->type) {
    case BTRFS_TREE_BLOCK_REF_KEY:
    case BTRFS_SHARED_BLOCK_REF_KEY: {
        k = mkkey(bytenr, r->type, r->type == BTRFS_TREE_BLOCK_REF_KEY ? r->root : r->parent);
        if (count == 1) {
            int ret = btrfs_insert_item(t, ext, &k, NULL, 0);
            return ret == -EEXIST ? -EIO : ret; /* a tree block ref is held once */
        }
        if (count == -1) {
            int ret = btrfs_delete_key(t, ext, &k);
            return ret == -ENOENT ? -EIO : ret;
        }
        return -EIO;
    }
    case BTRFS_SHARED_DATA_REF_KEY: {
        BtrfsSharedDataRef sr;
        k = mkkey(bytenr, BTRFS_SHARED_DATA_REF_KEY, r->parent);
        if (btrfs_lookup(b, ext, &k, &sr, sizeof sr, NULL) == 0) {
            int64_t n = (int64_t)sr.count + count;
            if (n < 0)
                return -EIO;
            if (n == 0)
                return btrfs_delete_key(t, ext, &k);
            sr.count = (uint32_t)n;
            return btrfs_update_item(t, ext, &k, &sr, sizeof sr);
        }
        if (count < 0)
            return -EIO;
        sr.count = (uint32_t)count;
        return btrfs_insert_item(t, ext, &k, &sr, sizeof sr);
    }
    case BTRFS_EXTENT_DATA_REF_KEY: {
        BtrfsExtentDataRef dr;
        if (find_data_ref(b, bytenr, r, &k, &dr) == 0) {
            int64_t n = (int64_t)dr.count + count;
            if (n < 0)
                return -EIO;
            if (n == 0)
                return btrfs_delete_key(t, ext, &k);
            dr.count = (uint32_t)n;
            return btrfs_update_item(t, ext, &k, &dr, sizeof dr);
        }
        if (count < 0)
            return -EIO;
        dr = (BtrfsExtentDataRef){ r->root, r->owner, r->offset, (uint32_t)count };
        k = mkkey(bytenr, BTRFS_EXTENT_DATA_REF_KEY, hash_extent_data_ref(r->root, r->owner, r->offset));
        for (;;) { /* a hash collision takes the next offset */
            int ret = btrfs_insert_item(t, ext, &k, &dr, sizeof dr);
            if (ret != -EEXIST)
                return ret;
            k.offset++;
        }
    }
    }
    return -EIO;
}

/* Apply one head: insert the extent item if it is new, add and drop its
 * back-references, and free the extent if no reference is left. */
int run_one_head(Trans *t, DelayedHead *h)
{
    Btrfs *b = t->b;
    BlockGroup *bg = find_block_group(b, h->bytenr);
    SpaceInfo *s;
    BtrfsKey k;
    BtrfsExtentItem ei;
    int64_t refs = 0, pos = 0;

    if (!bg)
        return -EIO;
    s = &b->space[space_of(bg->flags)];
    for (size_t i = 0; i < h->nrefs; i++)
        if (h->refs[i].count > 0)
            pos += h->refs[i].count;

    if (h->must_insert) {
        bg->reserved -= h->num_bytes;
        s->reserved -= h->num_bytes;
        if (pos == 0) {
            /* Allocated and dropped in the same transaction: never inserted. */
            if (h->is_data)
                TRY(csum_delete(t, h->bytenr, h->num_bytes));
            pin_extent(b, bg, h->bytenr, h->num_bytes);
            return 0;
        }
        for (size_t i = 0; i < h->nrefs; i++)
            refs += h->refs[i].count;
        if (refs <= 0)
            return -EIO;
        ei.refs = (uint64_t)refs;
        ei.generation = b->transid;
        ei.flags = (h->is_data ? BTRFS_EXTENT_FLAG_DATA : BTRFS_EXTENT_FLAG_TREE_BLOCK) | h->set_flags;
        k = h->is_data ? mkkey(h->bytenr, BTRFS_EXTENT_ITEM_KEY, h->num_bytes)
                       : mkkey(h->bytenr, BTRFS_METADATA_ITEM_KEY, (uint64_t)h->level);
        TRY(btrfs_insert_item(t, b->extent_root, &k, &ei, sizeof ei));
        bg->used += h->num_bytes;
        s->used += h->num_bytes;
        bg->dirty = 1;
        TRY(fst_remove(t, bg, h->bytenr, h->num_bytes));
        for (size_t i = 0; i < h->nrefs; i++)
            if (h->refs[i].count > 0)
                TRY(backref_mod(t, h->bytenr, &h->refs[i], h->refs[i].count));
        for (size_t i = 0; i < h->nrefs; i++)
            if (h->refs[i].count < 0)
                TRY(backref_mod(t, h->bytenr, &h->refs[i], h->refs[i].count));
        return 0;
    }

    if (find_extent_item(b, h->bytenr, h->level, h->is_data, h->num_bytes, &k, &ei) < 0)
        return -EIO; /* a reference to an extent that does not exist */
    refs = (int64_t)ei.refs;
    /* Adds first, so the count never touches 0 on the way. */
    for (size_t i = 0; i < h->nrefs; i++)
        if (h->refs[i].count > 0) {
            TRY(backref_mod(t, h->bytenr, &h->refs[i], h->refs[i].count));
            refs += h->refs[i].count;
        }
    for (size_t i = 0; i < h->nrefs; i++)
        if (h->refs[i].count < 0) {
            TRY(backref_mod(t, h->bytenr, &h->refs[i], h->refs[i].count));
            refs += h->refs[i].count;
        }
    if (refs < 0)
        return -EIO;
    if (refs == 0) {
        /* __btrfs_free_extent: the last reference went. */
        TRY(btrfs_delete_key(t, b->extent_root, &k));
        if (h->is_data)
            TRY(csum_delete(t, h->bytenr, h->num_bytes));
        TRY(fst_add(t, bg, h->bytenr, h->num_bytes));
        bg->used -= h->num_bytes;
        s->used -= h->num_bytes;
        bg->dirty = 1;
        pin_extent(b, bg, h->bytenr, h->num_bytes);
        return 0;
    }
    if ((uint64_t)refs != ei.refs || (ei.flags | h->set_flags) != ei.flags) {
        ei.refs = (uint64_t)refs;
        ei.flags |= h->set_flags;
        TRY(btrfs_update_item(t, b->extent_root, &k, &ei, sizeof ei));
    }
    return 0;
}
