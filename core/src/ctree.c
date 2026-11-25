/*
 * ctree.c - the copy-on-write B-tree (the kernel's fs/btrfs/ctree.c).
 *
 * A tree is a root block; leaves (level 0) hold sorted items, nodes hold
 * sorted (key, child, child generation) pointers. To change a tree, a
 * search walks down from the root and COPIES every block on the path that
 * was written in an earlier transaction (btrfs_cow_block), pointing each
 * copied parent at the copied child. So the committed tree is never
 * changed: the copies make a new tree that shares every block off the path.
 *
 * A block copied in the running transaction and not yet written can be
 * changed in place (should_cow_block), so a block is copied at most once
 * per transaction.
 *
 * When the block being copied is shared with a snapshot, the reference
 * counts of its children change: update_ref_for_cow, below, is the exact
 * btrfs rule.
 */
#include <errno.h>
#include <stdlib.h>

#include "ctree.h"

int btrfs_comp_keys(const BtrfsKey *a, const BtrfsKey *b)
{
    if (a->objectid != b->objectid)
        return a->objectid < b->objectid ? -1 : 1;
    if (a->type != b->type)
        return a->type < b->type ? -1 : 1;
    if (a->offset != b->offset)
        return a->offset < b->offset ? -1 : 1;
    return 0;
}

uint32_t leaf_data_size(const Btrfs *b)
{
    return b->nodesize - HDR_SIZE;
}

uint32_t max_node_ptrs(const Btrfs *b)
{
    return (b->nodesize - HDR_SIZE) / KEYPTR_SIZE;
}

/* Bytes used by items [start, start + nr): their headers and data. */
static uint32_t leaf_space_used(const ExtentBuffer *l, int start, int nr)
{
    if (nr <= 0)
        return 0;
    int end = start + nr - 1;
    uint32_t data = item_offset(l, start) + item_size(l, start) - item_offset(l, end);
    return data + (uint32_t)nr * ITEM_SIZE;
}

uint32_t leaf_free_space(const Btrfs *b, const ExtentBuffer *l)
{
    return leaf_data_size(b) - leaf_space_used(l, 0, (int)eb_nritems(l));
}

/* Offset of the lowest item data (the data grows down from the end). */
static uint32_t leaf_data_end(const Btrfs *b, const ExtentBuffer *l)
{
    uint32_t n = eb_nritems(l);
    return n ? item_offset(l, (int)n - 1) : leaf_data_size(b);
}

static void first_key(const ExtentBuffer *eb, BtrfsKey *k)
{
    if (eb_level(eb) == 0)
        item_key(eb, 0, k);
    else
        node_key(eb, 0, k);
}

/* 0 = found at *slot; 1 = not found, *slot is where it would go. */
static int bin_search(const ExtentBuffer *eb, const BtrfsKey *key, int *slot)
{
    int lo = 0, hi = (int)eb_nritems(eb), leaf = eb_level(eb) == 0;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        BtrfsKey k;
        if (leaf)
            item_key(eb, mid, &k);
        else
            node_key(eb, mid, &k);
        int c = btrfs_comp_keys(&k, key);
        if (c < 0)
            lo = mid + 1;
        else if (c > 0)
            hi = mid;
        else {
            *slot = mid;
            return 0;
        }
    }
    *slot = lo;
    return 1;
}

void btrfs_mark_dirty(Trans *t, ExtentBuffer *eb)
{
    eb_mark_dirty(t->b, eb);
    t->b->trans_dirty = 1;
}

/* ------------------------------------------------------ copy-on-write */

/* should_cow_block: a block written in this transaction and not on disk
 * yet may be changed in place, unless a snapshot was just taken. */
static int should_cow_block(Trans *t, Root *root, ExtentBuffer *eb)
{
    if (eb_gen(eb) == t->b->transid && !(eb_flags(eb) & BTRFS_HEADER_FLAG_WRITTEN) &&
        !root->force_cow)
        return 0;
    return 1;
}

/* btrfs_block_can_be_shared: only blocks of a subvolume tree that are at
 * least as old as its last snapshot can be shared. A root block never is:
 * a snapshot gets its own copy of the root. */
static int block_can_be_shared(Root *root, ExtentBuffer *eb)
{
    return root->shareable && eb != root->node && eb->start != root->commit_root &&
           eb_gen(eb) <= root->item.last_snapshot;
}

/*
 * update_ref_for_cow: `buf` is being replaced by its copy `cow` in `root`.
 *
 *   refs > 1 (another tree uses buf too; buf stays):
 *     - buf belongs to this root and its children's references are the
 *       implicit kind ("used by the tree that owns the parent"): those
 *       references now serve `cow`, so give buf its own: one SHARED ref
 *       per child, naming buf as the parent (FULL_BACKREF).
 *     - otherwise: give each child a reference from `cow`.
 *   refs == 1 (only this tree uses buf; it will be freed):
 *     - if buf's children hold SHARED refs naming buf, move them to `cow`.
 *     - otherwise `cow` simply takes over the children's references.
 */
static int update_ref_for_cow(Trans *t, Root *root, ExtentBuffer *buf, ExtentBuffer *cow,
                              int *last_ref)
{
    uint64_t refs = 1, flags = 0;
    uint64_t owner = eb_owner(buf);

    if (block_can_be_shared(root, buf))
        TRY(lookup_extent_info(t->b, buf->start, eb_level(buf), 0, &refs, &flags));
    if (refs == 0)
        return trans_abort(t, -EIO);

    *last_ref = 0;
    if (refs > 1) {
        if (owner == root->objectid && !(flags & BTRFS_BLOCK_FLAG_FULL_BACKREF)) {
            TRY(btrfs_inc_ref(t, root, buf, 1));
            TRY(set_extent_flags(t, buf, BTRFS_BLOCK_FLAG_FULL_BACKREF));
        } else {
            TRY(btrfs_inc_ref(t, root, cow, 0));
        }
    } else {
        if (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF) {
            TRY(btrfs_inc_ref(t, root, cow, 0));
            TRY(btrfs_dec_ref(t, root, buf, 1));
        }
        *last_ref = 1;
    }
    return 0;
}

/* btrfs_cow_block: make the copy of `buf` that this transaction may change,
 * and point the parent (or the root) at it. */
static int cow_block(Trans *t, Root *root, ExtentBuffer *buf, ExtentBuffer *parent, int pslot,
                     ExtentBuffer **out)
{
    Btrfs *b = t->b;
    ExtentBuffer *cow;
    int last_ref;

    if (!should_cow_block(t, root, buf)) {
        *out = buf;
        return 0;
    }
    TRY(alloc_tree_block(t, root, eb_level(buf), &cow));
    memcpy(cow->data, buf->data, b->nodesize);
    eb_set_bytenr(cow, cow->start);
    eb_set_gen(cow, b->transid);
    eb_set_owner(cow, root->objectid);
    eb_set_flags(cow, (eb_flags(buf) & ~(BTRFS_HEADER_FLAG_WRITTEN | BTRFS_HEADER_FLAG_RELOC)) |
                          (BTRFS_MIXED_BACKREF_REV << BTRFS_BACKREF_REV_SHIFT));

    TRY(update_ref_for_cow(t, root, buf, cow, &last_ref));

    if (buf == root->node) {
        root->node = cow;
        root->dirty = 1;
    } else {
        set_node_blockptr(parent, pslot, cow->start);
        set_node_ptr_gen(parent, pslot, b->transid);
        btrfs_mark_dirty(t, parent);
    }
    TRY(free_tree_block(t, root->objectid, buf, 0, last_ref));
    btrfs_mark_dirty(t, cow);
    b->stats.cow_copies++;
    *out = cow;
    return 0;
}

int btrfs_cow_root_node(Trans *t, Root *root)
{
    ExtentBuffer *eb;
    return cow_block(t, root, root->node, NULL, 0, &eb);
}

static int read_child(Btrfs *b, ExtentBuffer *parent, int slot, ExtentBuffer **out)
{
    return read_tree_block(b, node_blockptr(parent, slot), node_ptr_gen(parent, slot),
                           eb_level(parent) - 1, out);
}

/* Set the key at the path's slot from `level` up while it is slot 0. */
static void fixup_low_keys(Trans *t, Path *p, const BtrfsKey *key, int level)
{
    for (int i = level; i < BTRFS_MAX_LEVEL && p->nodes[i]; i++) {
        set_node_key(p->nodes[i], p->slots[i], key);
        btrfs_mark_dirty(t, p->nodes[i]);
        if (p->slots[i] != 0)
            break;
    }
}

/* ------------------------------------------------- growing the tree */

/* A new root one level up, pointing at the old one. */
static int insert_new_root(Trans *t, Root *root, Path *p, int level)
{
    ExtentBuffer *lower = p->nodes[level - 1], *c;
    BtrfsKey k;

    TRY(alloc_tree_block(t, root, level, &c));
    first_key(lower, &k);
    eb_set_nritems(c, 1);
    set_node_key(c, 0, &k);
    set_node_blockptr(c, 0, lower->start);
    set_node_ptr_gen(c, 0, eb_gen(lower));
    btrfs_mark_dirty(t, c);
    root->node = c;
    root->dirty = 1;
    p->nodes[level] = c;
    p->slots[level] = 0;
    return 0;
}

static void insert_ptr(Trans *t, Path *p, int level, int slot, const BtrfsKey *key,
                       uint64_t bytenr, uint64_t gen)
{
    ExtentBuffer *n = p->nodes[level];
    uint32_t nr = eb_nritems(n);
    uint8_t *base = n->data + HDR_SIZE;
    memmove(base + (size_t)(slot + 1) * KEYPTR_SIZE, base + (size_t)slot * KEYPTR_SIZE,
            (size_t)(nr - (uint32_t)slot) * KEYPTR_SIZE);
    eb_set_nritems(n, nr + 1);
    set_node_key(n, slot, key);
    set_node_blockptr(n, slot, bytenr);
    set_node_ptr_gen(n, slot, gen);
    btrfs_mark_dirty(t, n);
    if (slot == 0)
        fixup_low_keys(t, p, key, level + 1);
}

/* Split a full node in two (it is on the path at `level`). */
static int split_node(Trans *t, Root *root, Path *p, int level)
{
    ExtentBuffer *c = p->nodes[level], *split;
    BtrfsKey k;

    if (!p->nodes[level + 1])
        TRY(insert_new_root(t, root, p, level + 1));
    uint32_t nr = eb_nritems(c), mid = (nr + 1) / 2;
    TRY(alloc_tree_block(t, root, level, &split));
    memcpy(split->data + HDR_SIZE, c->data + HDR_SIZE + (size_t)mid * KEYPTR_SIZE,
           (size_t)(nr - mid) * KEYPTR_SIZE);
    eb_set_nritems(split, nr - mid);
    eb_set_nritems(c, mid);
    btrfs_mark_dirty(t, c);
    btrfs_mark_dirty(t, split);
    node_key(split, 0, &k);
    insert_ptr(t, p, level + 1, p->slots[level + 1] + 1, &k, split->start, t->b->transid);
    if ((uint32_t)p->slots[level] >= mid) {
        p->nodes[level] = split;
        p->slots[level] -= (int)mid;
        p->slots[level + 1]++;
    }
    return 0;
}

/* Copy items [start, start + nr) of `src` to the end of `dst`. */
static void append_items(const Btrfs *b, ExtentBuffer *dst, const ExtentBuffer *src, int start, int nr)
{
    if (nr <= 0)
        return;
    uint32_t dn = eb_nritems(dst);
    int last = start + nr - 1;
    uint32_t src_end = item_offset(src, start) + item_size(src, start);
    uint32_t src_low = item_offset(src, last);
    uint32_t len = src_end - src_low;
    uint32_t dst_end = leaf_data_end(b, dst);

    memcpy(dst->data + HDR_SIZE + dst_end - len, src->data + HDR_SIZE + src_low, len);
    memcpy(dst->data + HDR_SIZE + (size_t)dn * ITEM_SIZE, src->data + HDR_SIZE + (size_t)start * ITEM_SIZE,
           (size_t)nr * ITEM_SIZE);
    for (int i = 0; i < nr; i++) {
        int d = (int)dn + i;
        set_item_offset(dst, d, item_offset(dst, d) - src_end + dst_end);
    }
    eb_set_nritems(dst, dn + (uint32_t)nr);
}

/* Move items [mid, n) of the path's leaf into a new leaf to its right. */
static int split_leaf_at(Trans *t, Root *root, Path *p, int mid, const BtrfsKey *empty_key)
{
    Btrfs *b = t->b;
    ExtentBuffer *l = p->nodes[0], *right;
    int n = (int)eb_nritems(l);
    BtrfsKey k;

    TRY(alloc_tree_block(t, root, 0, &right));
    append_items(b, right, l, mid, n - mid);
    eb_set_nritems(l, (uint32_t)mid);
    btrfs_mark_dirty(t, l);
    btrfs_mark_dirty(t, right);
    if (n - mid > 0)
        item_key(right, 0, &k);
    else
        k = *empty_key; /* an empty right leaf will receive the new item */
    insert_ptr(t, p, 1, p->slots[1] + 1, &k, right->start, b->transid);
    return 0;
}

/*
 * split_leaf: make room for `need` bytes at the path's slot. extend = 0:
 * a new item goes at the slot; extend = 1: the item at the slot grows.
 * Picks the split point closest to half that fits; if none does, splits
 * the item away from its neighbours and tries again.
 */
static int split_leaf(Trans *t, Root *root, const BtrfsKey *key, Path *p, uint32_t need, int extend)
{
    Btrfs *b = t->b;

    for (;;) {
        ExtentBuffer *l = p->nodes[0];
        int n = (int)eb_nritems(l), slot = p->slots[0];
        uint32_t cap = leaf_data_size(b);
        int best_mid = -1, best_left = 0;
        int64_t best_diff = INT64_MAX;

        if (leaf_free_space(b, l) >= need)
            return 0;
        if (!p->nodes[1])
            TRY(insert_new_root(t, root, p, 1));

        for (int mid = 0; mid <= n; mid++) {
            uint32_t lu = leaf_space_used(l, 0, mid), ru = leaf_space_used(l, mid, n - mid);
            for (int side = 0; side < 2; side++) { /* 0 = left, 1 = right */
                int left = side == 0;
                if (extend) {
                    if (left != (slot < mid))
                        continue;
                    if (mid == 0 || mid == n)
                        continue; /* both leaves keep items */
                } else {
                    if (left ? slot > mid : slot < mid)
                        continue;
                    if ((mid == 0 && !left) || (mid == n && left))
                        continue; /* never leave an empty leaf */
                }
                uint32_t a = lu + (left ? need : 0), c = ru + (left ? 0 : need);
                if (a > cap || c > cap)
                    continue;
                int64_t diff = (int64_t)a - (int64_t)c;
                if (diff < 0)
                    diff = -diff;
                if (diff < best_diff) {
                    best_diff = diff;
                    best_mid = mid;
                    best_left = left;
                }
            }
        }
        if (best_mid >= 0) {
            TRY(split_leaf_at(t, root, p, best_mid, key));
            int goes_right = extend ? slot >= best_mid : !best_left;
            if (goes_right) {
                ExtentBuffer *r;
                TRY(read_child(b, p->nodes[1], p->slots[1] + 1, &r));
                p->nodes[0] = r;
                p->slots[0] = slot - best_mid;
                p->slots[1]++;
            }
            return 0;
        }
        /* No single split works: cut next to the item and try again. */
        if (extend) {
            if (n < 2)
                return -EOVERFLOW; /* the item alone does not fit */
            int mid = slot > 0 ? slot : 1;
            TRY(split_leaf_at(t, root, p, mid, key));
            if (slot >= mid) {
                ExtentBuffer *r;
                TRY(read_child(b, p->nodes[1], p->slots[1] + 1, &r));
                p->nodes[0] = r;
                p->slots[0] = slot - mid;
                p->slots[1]++;
            }
        } else {
            TRY(split_leaf_at(t, root, p, slot, key)); /* the item goes at the end of the left */
        }
    }
}

/* -------------------------------------------------- shrinking the tree */

static int del_ptr(Trans *t, Root *root, Path *p, int level, int slot);

/* Free a block that has nothing in it any more (its children moved). */
static int free_empty_block(Trans *t, Root *root, ExtentBuffer *eb)
{
    return free_tree_block(t, root->objectid, eb, 0, 1);
}

static int del_ptr(Trans *t, Root *root, Path *p, int level, int slot)
{
    ExtentBuffer *n = p->nodes[level];
    uint32_t nr = eb_nritems(n);
    uint8_t *base = n->data + HDR_SIZE;

    memmove(base + (size_t)slot * KEYPTR_SIZE, base + (size_t)(slot + 1) * KEYPTR_SIZE,
            (size_t)(nr - (uint32_t)slot - 1) * KEYPTR_SIZE);
    eb_set_nritems(n, nr - 1);
    btrfs_mark_dirty(t, n);
    if (nr - 1 == 0) {
        if (n == root->node) {
            eb_set_level(n, 0); /* the tree is empty: the root becomes an empty leaf */
            return 0;
        }
        TRY(del_ptr(t, root, p, level + 1, p->slots[level + 1]));
        return free_empty_block(t, root, n);
    }
    if (slot == 0) {
        BtrfsKey k;
        node_key(n, 0, &k);
        fixup_low_keys(t, p, &k, level + 1);
    }
    return 0;
}

static void move_ptrs(ExtentBuffer *dst, uint32_t dpos, const ExtentBuffer *src, uint32_t spos, uint32_t nr)
{
    uint8_t *d = dst->data + HDR_SIZE, *s = src->data + HDR_SIZE;
    uint32_t dn = eb_nritems(dst);
    memmove(d + (size_t)(dpos + nr) * KEYPTR_SIZE, d + (size_t)dpos * KEYPTR_SIZE,
            (size_t)(dn - dpos) * KEYPTR_SIZE);
    memcpy(d + (size_t)dpos * KEYPTR_SIZE, s + (size_t)spos * KEYPTR_SIZE, (size_t)nr * KEYPTR_SIZE);
    eb_set_nritems(dst, dn + nr);
}

static void cut_ptrs(ExtentBuffer *eb, uint32_t pos, uint32_t nr)
{
    uint8_t *d = eb->data + HDR_SIZE;
    uint32_t n = eb_nritems(eb);
    memmove(d + (size_t)pos * KEYPTR_SIZE, d + (size_t)(pos + nr) * KEYPTR_SIZE,
            (size_t)(n - pos - nr) * KEYPTR_SIZE);
    eb_set_nritems(eb, n - nr);
}

/*
 * balance_level: on the way down to a delete, a node with few pointers is
 * merged with a neighbour or takes pointers from it; a root with a single
 * child is replaced by that child. *changed = 1 means the path is stale
 * and the search starts again.
 */
static int balance_level(Trans *t, Root *root, Path *p, int level, int *changed)
{
    Btrfs *b = t->b;
    ExtentBuffer *mid = p->nodes[level], *parent = p->nodes[level + 1];
    uint32_t max = max_node_ptrs(b);
    *changed = 0;

    if (!parent) {
        if (eb_nritems(mid) != 1)
            return 0;
        /* Promote the only child to be the root. */
        ExtentBuffer *child;
        TRY(read_child(b, mid, 0, &child));
        TRY(cow_block(t, root, child, mid, 0, &child));
        root->node = child;
        root->dirty = 1;
        TRY(free_empty_block(t, root, mid));
        *changed = 1;
        return 0;
    }
    if (eb_nritems(mid) > max / 4)
        return 0;

    int pslot = p->slots[level + 1];
    ExtentBuffer *left = NULL, *right = NULL;
    BtrfsKey k;
    if (pslot > 0) {
        TRY(read_child(b, parent, pslot - 1, &left));
        TRY(cow_block(t, root, left, parent, pslot - 1, &left));
    }
    if ((uint32_t)pslot + 1 < eb_nritems(parent)) {
        TRY(read_child(b, parent, pslot + 1, &right));
        TRY(cow_block(t, root, right, parent, pslot + 1, &right));
    }
    uint32_t nm = eb_nritems(mid);

    if (left && eb_nritems(left) + nm <= max) {
        move_ptrs(left, eb_nritems(left), mid, 0, nm); /* mid goes into left */
        eb_set_nritems(mid, 0);
        btrfs_mark_dirty(t, left);
        TRY(del_ptr(t, root, p, level + 1, pslot));
        TRY(free_empty_block(t, root, mid));
    } else if (right && nm + eb_nritems(right) <= max) {
        move_ptrs(mid, nm, right, 0, eb_nritems(right)); /* right goes into mid */
        eb_set_nritems(right, 0);
        btrfs_mark_dirty(t, mid);
        TRY(del_ptr(t, root, p, level + 1, pslot + 1));
        TRY(free_empty_block(t, root, right));
    } else if (left) {
        uint32_t nl = eb_nritems(left), want = (nl + nm) / 2 - nm;
        move_ptrs(mid, 0, left, nl - want, want); /* take from the end of left */
        eb_set_nritems(left, nl - want);
        btrfs_mark_dirty(t, left);
        btrfs_mark_dirty(t, mid);
        node_key(mid, 0, &k);
        set_node_key(parent, pslot, &k);
        btrfs_mark_dirty(t, parent);
    } else if (right) {
        uint32_t nr = eb_nritems(right), want = (nr + nm) / 2 - nm;
        move_ptrs(mid, nm, right, 0, want); /* take from the front of right */
        cut_ptrs(right, 0, want);
        btrfs_mark_dirty(t, right);
        btrfs_mark_dirty(t, mid);
        node_key(right, 0, &k);
        set_node_key(parent, pslot + 1, &k);
        btrfs_mark_dirty(t, parent);
    } else {
        return 0;
    }
    *changed = 1;
    return 0;
}

/* --------------------------------------------------------------- search */

int btrfs_search_slot(Trans *t, Root *root, const BtrfsKey *key, Path *p, int ins_len, int cow)
{
    Btrfs *b = root->fs_info;
again:
    path_init(p);
    ExtentBuffer *eb = root->node;
    if (cow)
        TRY(cow_block(t, root, eb, NULL, 0, &eb));
    for (;;) {
        int level = eb_level(eb), slot;
        p->nodes[level] = eb;
        int ret = bin_search(eb, key, &slot);
        if (level > 0) {
            if (ret && slot > 0)
                slot--;
            p->slots[level] = slot;
            if (ins_len > 0 && eb_nritems(eb) + 3 >= max_node_ptrs(b)) {
                TRY(split_node(t, root, p, level));
                eb = p->nodes[level];
                slot = p->slots[level];
            } else if (ins_len < 0 && eb_nritems(eb) < max_node_ptrs(b) / 2) {
                int changed;
                TRY(balance_level(t, root, p, level, &changed));
                if (changed)
                    goto again;
            }
            ExtentBuffer *child;
            TRY(read_child(b, eb, slot, &child));
            if (cow)
                TRY(cow_block(t, root, child, eb, slot, &child));
            eb = child;
        } else {
            p->slots[0] = slot;
            if (ins_len > 0 && leaf_free_space(b, eb) < (uint32_t)ins_len)
                TRY(split_leaf(t, root, key, p, (uint32_t)ins_len, ret == 0));
            return ret;
        }
    }
}

/* A search that changes nothing: no transaction needed. */
static int search_ro(Btrfs *b, Root *root, const BtrfsKey *key, Path *p)
{
    path_init(p);
    ExtentBuffer *eb = root->node;
    for (;;) {
        int level = eb_level(eb), slot;
        p->nodes[level] = eb;
        int ret = bin_search(eb, key, &slot);
        if (level == 0) {
            p->slots[0] = slot;
            return ret;
        }
        if (ret && slot > 0)
            slot--;
        p->slots[level] = slot;
        TRY(read_child(b, eb, slot, &eb));
    }
}

int btrfs_next_leaf(Btrfs *b, Path *p)
{
    int level = 1, slot = 0;
    for (; level < BTRFS_MAX_LEVEL; level++) {
        if (!p->nodes[level])
            return 1;
        slot = p->slots[level] + 1;
        if ((uint32_t)slot < eb_nritems(p->nodes[level]))
            break;
    }
    if (level == BTRFS_MAX_LEVEL)
        return 1;
    p->slots[level] = slot;
    for (int l = level - 1; l >= 0; l--) {
        TRY(read_child(b, p->nodes[l + 1], p->slots[l + 1], &p->nodes[l]));
        p->slots[l] = 0;
    }
    return 0;
}

int btrfs_next_item(Btrfs *b, Path *p)
{
    p->slots[0]++;
    while ((uint32_t)p->slots[0] >= eb_nritems(p->nodes[0])) {
        int r = btrfs_next_leaf(b, p);
        if (r)
            return r;
    }
    return 0;
}

int btrfs_prev_item(Btrfs *b, Path *p)
{
    if (p->slots[0] > 0) {
        p->slots[0]--;
        return 0;
    }
    int level = 1;
    for (; level < BTRFS_MAX_LEVEL; level++) {
        if (!p->nodes[level])
            return 1;
        if (p->slots[level] > 0)
            break;
    }
    if (level == BTRFS_MAX_LEVEL)
        return 1;
    p->slots[level]--;
    for (int l = level - 1; l >= 0; l--) {
        TRY(read_child(b, p->nodes[l + 1], p->slots[l + 1], &p->nodes[l]));
        p->slots[l] = (int)eb_nritems(p->nodes[l]) - 1;
    }
    if (p->slots[0] < 0)
        return 1;
    return 0;
}

int btrfs_search_forward(Btrfs *b, Root *root, const BtrfsKey *k, Path *p)
{
    TRY(search_ro(b, root, k, p));
    if ((uint32_t)p->slots[0] >= eb_nritems(p->nodes[0]))
        return btrfs_next_leaf(b, p) ? 1 : 0;
    return 0;
}

int btrfs_lookup(Btrfs *b, Root *root, const BtrfsKey *k, void *buf, uint32_t len, uint32_t *size)
{
    Path p;
    int r = search_ro(b, root, k, &p);
    if (r < 0)
        return r;
    if (r == 1)
        return -ENOENT;
    uint32_t sz = item_size(p.nodes[0], p.slots[0]);
    if (size)
        *size = sz;
    if (buf)
        memcpy(buf, item_ptr(p.nodes[0], p.slots[0]), sz < len ? sz : len);
    return 0;
}

/* --------------------------------------------------- insert and delete */

int btrfs_insert_empty_item(Trans *t, Root *root, Path *p, const BtrfsKey *key, uint32_t size)
{
    Btrfs *b = t->b;
    if (size + ITEM_SIZE > leaf_data_size(b))
        return -EOVERFLOW;
    int r = btrfs_search_slot(t, root, key, p, (int)(size + ITEM_SIZE), 1);
    if (r < 0)
        return r;
    if (r == 0)
        return -EEXIST;

    ExtentBuffer *l = p->nodes[0];
    int slot = p->slots[0];
    uint32_t n = eb_nritems(l), data_end = leaf_data_end(b, l), off;

    if ((uint32_t)slot != n) {
        uint32_t old_data = item_offset(l, slot) + item_size(l, slot);
        for (uint32_t i = (uint32_t)slot; i < n; i++)
            set_item_offset(l, (int)i, item_offset(l, (int)i) - size);
        uint8_t *base = l->data + HDR_SIZE;
        memmove(base + (size_t)(slot + 1) * ITEM_SIZE, base + (size_t)slot * ITEM_SIZE,
                (size_t)(n - (uint32_t)slot) * ITEM_SIZE);
        memmove(base + data_end - size, base + data_end, old_data - data_end);
        off = old_data - size;
    } else {
        off = data_end - size;
    }
    set_item_key(l, slot, key);
    set_item_offset(l, slot, off);
    set_item_size(l, slot, size);
    memset(l->data + HDR_SIZE + off, 0, size);
    eb_set_nritems(l, n + 1);
    btrfs_mark_dirty(t, l);
    if (slot == 0)
        fixup_low_keys(t, p, key, 1);
    return 0;
}

int btrfs_insert_item(Trans *t, Root *root, const BtrfsKey *key, const void *data, uint32_t size)
{
    Path p;
    TRY(btrfs_insert_empty_item(t, root, &p, key, size));
    if (size)
        memcpy(item_ptr(p.nodes[0], p.slots[0]), data, size);
    return 0;
}

/* After deleting from a leaf: merge it into a neighbour if both fit in one. */
static int merge_leaf(Trans *t, Root *root, Path *p)
{
    Btrfs *b = t->b;
    ExtentBuffer *l = p->nodes[0], *parent = p->nodes[1];
    int pslot = p->slots[1];
    uint32_t used = leaf_space_used(l, 0, (int)eb_nritems(l)), cap = leaf_data_size(b);

    if (!parent || used >= cap / 3)
        return 0;
    if (pslot > 0) {
        ExtentBuffer *left;
        TRY(read_child(b, parent, pslot - 1, &left));
        if (leaf_space_used(left, 0, (int)eb_nritems(left)) + used <= cap) {
            TRY(cow_block(t, root, left, parent, pslot - 1, &left));
            append_items(b, left, l, 0, (int)eb_nritems(l));
            eb_set_nritems(l, 0);
            btrfs_mark_dirty(t, left);
            TRY(del_ptr(t, root, p, 1, pslot));
            return free_empty_block(t, root, l);
        }
    }
    if ((uint32_t)pslot + 1 < eb_nritems(parent)) {
        ExtentBuffer *right;
        TRY(read_child(b, parent, pslot + 1, &right));
        if (used + leaf_space_used(right, 0, (int)eb_nritems(right)) <= cap) {
            TRY(cow_block(t, root, right, parent, pslot + 1, &right));
            append_items(b, l, right, 0, (int)eb_nritems(right));
            eb_set_nritems(right, 0);
            btrfs_mark_dirty(t, l);
            TRY(del_ptr(t, root, p, 1, pslot + 1));
            return free_empty_block(t, root, right);
        }
    }
    return 0;
}

int btrfs_del_item(Trans *t, Root *root, Path *p)
{
    Btrfs *b = t->b;
    ExtentBuffer *l = p->nodes[0];
    int slot = p->slots[0];
    uint32_t n = eb_nritems(l), dsize = item_size(l, slot);

    if ((uint32_t)slot != n - 1) {
        uint32_t data_end = leaf_data_end(b, l);
        uint8_t *base = l->data + HDR_SIZE;
        memmove(base + data_end + dsize, base + data_end, item_offset(l, slot) - data_end);
        for (uint32_t i = (uint32_t)slot + 1; i < n; i++)
            set_item_offset(l, (int)i, item_offset(l, (int)i) + dsize);
        memmove(base + (size_t)slot * ITEM_SIZE, base + (size_t)(slot + 1) * ITEM_SIZE,
                (size_t)(n - (uint32_t)slot - 1) * ITEM_SIZE);
    }
    eb_set_nritems(l, n - 1);
    btrfs_mark_dirty(t, l);
    if (n - 1 == 0) {
        if (l == root->node)
            return 0;
        TRY(del_ptr(t, root, p, 1, p->slots[1]));
        return free_empty_block(t, root, l);
    }
    if (slot == 0) {
        BtrfsKey k;
        item_key(l, 0, &k);
        fixup_low_keys(t, p, &k, 1);
    }
    return merge_leaf(t, root, p);
}

void btrfs_extend_item(Trans *t, Path *p, uint32_t extra)
{
    Btrfs *b = t->b;
    ExtentBuffer *l = p->nodes[0];
    int slot = p->slots[0];
    uint32_t n = eb_nritems(l), data_end = leaf_data_end(b, l);
    uint32_t off = item_offset(l, slot), size = item_size(l, slot);
    uint8_t *base = l->data + HDR_SIZE;

    memmove(base + data_end - extra, base + data_end, off + size - data_end);
    for (uint32_t i = (uint32_t)slot; i < n; i++)
        set_item_offset(l, (int)i, item_offset(l, (int)i) - extra);
    set_item_size(l, slot, size + extra);
    memset(base + off + size - extra, 0, extra);
    btrfs_mark_dirty(t, l);
}

void btrfs_truncate_item(Trans *t, Path *p, uint32_t new_size, int from_end)
{
    Btrfs *b = t->b;
    ExtentBuffer *l = p->nodes[0];
    int slot = p->slots[0];
    uint32_t n = eb_nritems(l), data_end = leaf_data_end(b, l);
    uint32_t off = item_offset(l, slot), size = item_size(l, slot), cut = size - new_size;
    uint8_t *base = l->data + HDR_SIZE;

    if (from_end) {
        memmove(base + data_end + cut, base + data_end, off + new_size - data_end);
        for (uint32_t i = (uint32_t)slot; i < n; i++)
            set_item_offset(l, (int)i, item_offset(l, (int)i) + cut);
    } else {
        memmove(base + data_end + cut, base + data_end, off - data_end);
        for (uint32_t i = (uint32_t)slot + 1; i < n; i++)
            set_item_offset(l, (int)i, item_offset(l, (int)i) + cut);
        set_item_offset(l, slot, off + cut);
    }
    set_item_size(l, slot, new_size);
    btrfs_mark_dirty(t, l);
}

void btrfs_set_item_key_safe(Trans *t, Path *p, const BtrfsKey *k)
{
    set_item_key(p->nodes[0], p->slots[0], k);
    btrfs_mark_dirty(t, p->nodes[0]);
    if (p->slots[0] == 0)
        fixup_low_keys(t, p, k, 1);
}

/* Overwrite the data of an existing item (its size must not change). */
int btrfs_update_item(Trans *t, Root *root, const BtrfsKey *k, const void *data, uint32_t len)
{
    Path p;
    int r = btrfs_search_slot(t, root, k, &p, 0, 1);
    if (r < 0)
        return r;
    if (r == 1 || item_size(p.nodes[0], p.slots[0]) != len)
        return -ENOENT;
    memcpy(item_ptr(p.nodes[0], p.slots[0]), data, len);
    btrfs_mark_dirty(t, p.nodes[0]);
    return 0;
}

int btrfs_delete_key(Trans *t, Root *root, const BtrfsKey *k)
{
    Path p;
    int r = btrfs_search_slot(t, root, k, &p, -1, 1);
    if (r < 0)
        return r;
    if (r == 1)
        return -ENOENT;
    return btrfs_del_item(t, root, &p);
}

int btrfs_search_back(Btrfs *b, Root *root, const BtrfsKey *k, Path *p)
{
    int r = search_ro(b, root, k, p);
    if (r <= 0)
        return r; /* exact match, or an error */
    return btrfs_prev_item(b, p);
}
