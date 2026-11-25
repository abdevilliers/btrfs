/*
 * check.c - btrfs check: read the committed filesystem and check it all,
 * from scratch (btrfs-progs check/main.c, in miniature).
 *
 *   1. Every tree block reachable from the superblock: checksum, bytenr,
 *      fsid, generation (as its parent expects), level, key order, item
 *      bounds, and each node key equal to its child's first key.
 *   2. References: walking every tree gives the back-references each
 *      extent SHOULD have (a block's children are referenced as
 *      TREE_BLOCK_REF of the block's owner, or SHARED_BLOCK_REF of the
 *      block if it has FULL_BACKREF; file extents likewise as
 *      EXTENT_DATA_REF or SHARED_DATA_REF). They must equal exactly the
 *      back-reference items in the extent tree, and each extent item's
 *      refs must be their sum. No extent may be leaked or overlap another.
 *   3. Block groups: every extent inside one of the right type; `used`
 *      equals the extents in it; superblock bytes_used is the total.
 *   4. The free space tree is exactly each block group minus its extents.
 *   5. The csum tree covers exactly the data extents, and every data
 *      sector matches its checksum (a scrub).
 *   6. Each subvolume: inode items, nlink against INODE_REFs, DIR_ITEM /
 *      DIR_INDEX / INODE_REF agreeing, directory sizes, file extents
 *      (sorted, no overlap, inline rules, nbytes), subvolume entries
 *      against ROOT_REF / ROOT_BACKREF, and the uuid tree.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "ctree.h"

typedef struct {
    uint8_t type;
    uint64_t root, parent, owner, offset;
    uint64_t count;
} Ref;

typedef struct {
    uint64_t bytenr;
    uint64_t key_offset; /* data: length; metadata: level */
    int is_data, has_item, reached;
    uint64_t refs, flags;
    Ref *have, *want;
    size_t nhave, nwant, chave, cwant;
} Rec;

typedef struct {
    Btrfs *b;
    int verbose, errors;
    Rec **recs;
    size_t nrecs, crecs;
    size_t *index; /* open addressing into recs */
    size_t nindex;
    uint64_t *visited;
    size_t nvisited, cvisited, *vindex, nvindex;
} Check;

static void problem(Check *c, const char *fmt, ...)
{
    c->errors++;
    if (c->verbose) {
        va_list ap;
        va_start(ap, fmt);
        printf("  problem: ");
        vprintf(fmt, ap);
        printf("\n");
        va_end(ap);
    }
}

/* --------------------------------------------------- a small hash map */

static size_t hslot(uint64_t v, size_t n)
{
    return (size_t)((v >> 12) * 11400714819323198485ULL >> 20) & (n - 1);
}

static int rehash(Check *c)
{
    size_t n = c->nindex ? c->nindex * 2 : 4096;
    size_t *ni = malloc(n * sizeof *ni);
    if (!ni)
        return -ENOMEM;
    for (size_t i = 0; i < n; i++)
        ni[i] = (size_t)-1;
    for (size_t r = 0; r < c->nrecs; r++) {
        size_t h = hslot(c->recs[r]->bytenr, n);
        while (ni[h] != (size_t)-1)
            h = (h + 1) & (n - 1);
        ni[h] = r;
    }
    free(c->index);
    c->index = ni;
    c->nindex = n;
    return 0;
}

static Rec *rec_get(Check *c, uint64_t bytenr, int create)
{
    if (c->nindex) {
        for (size_t h = hslot(bytenr, c->nindex); c->index[h] != (size_t)-1; h = (h + 1) & (c->nindex - 1))
            if (c->recs[c->index[h]]->bytenr == bytenr)
                return c->recs[c->index[h]];
    }
    if (!create)
        return NULL;
    if ((c->nrecs + 1) * 2 > c->nindex && rehash(c) < 0)
        return NULL;
    if (c->nrecs == c->crecs) {
        size_t nc = c->crecs ? c->crecs * 2 : 1024;
        Rec **nr = realloc(c->recs, nc * sizeof *nr);
        if (!nr)
            return NULL;
        c->recs = nr;
        c->crecs = nc;
    }
    Rec *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->bytenr = bytenr;
    c->recs[c->nrecs] = r;
    size_t h = hslot(bytenr, c->nindex);
    while (c->index[h] != (size_t)-1)
        h = (h + 1) & (c->nindex - 1);
    c->index[h] = c->nrecs++;
    return r;
}

static int visit(Check *c, uint64_t bytenr) /* 1 = first time */
{
    if ((c->nvisited + 1) * 2 > c->nvindex) {
        size_t n = c->nvindex ? c->nvindex * 2 : 4096;
        size_t *ni = malloc(n * sizeof *ni);
        if (!ni)
            return 0;
        for (size_t i = 0; i < n; i++)
            ni[i] = (size_t)-1;
        for (size_t i = 0; i < c->nvisited; i++) {
            size_t h = hslot(c->visited[i], n);
            while (ni[h] != (size_t)-1)
                h = (h + 1) & (n - 1);
            ni[h] = i;
        }
        free(c->vindex);
        c->vindex = ni;
        c->nvindex = n;
    }
    size_t h = hslot(bytenr, c->nvindex);
    for (; c->vindex[h] != (size_t)-1; h = (h + 1) & (c->nvindex - 1))
        if (c->visited[c->vindex[h]] == bytenr)
            return 0;
    if (c->nvisited == c->cvisited) {
        size_t nc = c->cvisited ? c->cvisited * 2 : 1024;
        uint64_t *nv = realloc(c->visited, nc * sizeof *nv);
        if (!nv)
            return 0;
        c->visited = nv;
        c->cvisited = nc;
    }
    c->visited[c->nvisited] = bytenr;
    c->vindex[h] = c->nvisited++;
    return 1;
}

static int same(const Ref *a, const Ref *b)
{
    return a->type == b->type && a->root == b->root && a->parent == b->parent && a->owner == b->owner &&
           a->offset == b->offset;
}

static void add_ref(Ref **list, size_t *n, size_t *cap, const Ref *r)
{
    for (size_t i = 0; i < *n; i++)
        if (same(&(*list)[i], r)) {
            (*list)[i].count += r->count;
            return;
        }
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 4;
        Ref *nl = realloc(*list, nc * sizeof *nl);
        if (!nl)
            return;
        *list = nl;
        *cap = nc;
    }
    (*list)[(*n)++] = *r;
}

static void want(Check *c, uint64_t bytenr, int is_data, uint64_t key_offset, const Ref *r)
{
    Rec *e = rec_get(c, bytenr, 1);
    if (!e)
        return;
    if (is_data && e->has_item && e->key_offset != key_offset)
        problem(c, "data extent %llu: a file extent says length %llu, the extent item %llu",
                (unsigned long long)bytenr, (unsigned long long)key_offset, (unsigned long long)e->key_offset);
    add_ref(&e->want, &e->nwant, &e->cwant, r);
}

/* ------------------------------------------------- 1. the extent tree */

static void scan_extent_tree(Check *c)
{
    Btrfs *b = c->b;
    Path p;
    BtrfsKey k = mkkey(0, 0, 0), f;
    int r = btrfs_search_forward(b, b->extent_root, &k, &p);
    while (r == 0) {
        ExtentBuffer *l = p.nodes[0];
        int s = p.slots[0];
        item_key(l, s, &f);
        const uint8_t *d = item_ptr(l, s);
        uint32_t size = item_size(l, s);
        Ref ref = { f.type, 0, 0, 0, 0, 1 };
        Rec *e = NULL;
        switch (f.type) {
        case BTRFS_EXTENT_ITEM_KEY:
        case BTRFS_METADATA_ITEM_KEY: {
            e = rec_get(c, f.objectid, 1);
            if (!e)
                break;
            if (e->has_item)
                problem(c, "two extent items at %llu", (unsigned long long)f.objectid);
            BtrfsExtentItem ei;
            memcpy(&ei, d, sizeof ei);
            e->has_item = 1;
            e->is_data = f.type == BTRFS_EXTENT_ITEM_KEY;
            e->key_offset = f.offset;
            e->refs = ei.refs;
            e->flags = ei.flags;
            if (e->is_data != ((ei.flags & BTRFS_EXTENT_FLAG_DATA) != 0))
                problem(c, "extent %llu: flags do not match its item type", (unsigned long long)f.objectid);
            e = NULL;
            break;
        }
        case BTRFS_TREE_BLOCK_REF_KEY:
            ref.root = f.offset;
            e = rec_get(c, f.objectid, 1);
            break;
        case BTRFS_SHARED_BLOCK_REF_KEY:
            ref.parent = f.offset;
            e = rec_get(c, f.objectid, 1);
            break;
        case BTRFS_SHARED_DATA_REF_KEY: {
            BtrfsSharedDataRef sr;
            memcpy(&sr, d, sizeof sr);
            ref.parent = f.offset;
            ref.count = sr.count;
            e = rec_get(c, f.objectid, 1);
            break;
        }
        case BTRFS_EXTENT_DATA_REF_KEY: {
            BtrfsExtentDataRef dr;
            memcpy(&dr, d, sizeof dr);
            ref.root = dr.root;
            ref.owner = dr.objectid;
            ref.offset = dr.offset;
            ref.count = dr.count;
            if (f.offset < hash_extent_data_ref(dr.root, dr.objectid, dr.offset))
                problem(c, "data ref of %llu is before its hash", (unsigned long long)f.objectid);
            e = rec_get(c, f.objectid, 1);
            break;
        }
        case BTRFS_BLOCK_GROUP_ITEM_KEY:
            break;
        default:
            problem(c, "unknown item type %u in the extent tree", f.type);
        }
        (void)size;
        if (e)
            add_ref(&e->have, &e->nhave, &e->chave, &ref);
        r = btrfs_next_item(b, &p);
    }
    if (r < 0)
        problem(c, "cannot read the extent tree");
}

/* ------------------------------------------------------ 2. the trees */

static void check_leaf(Check *c, ExtentBuffer *l)
{
    uint32_t n = eb_nritems(l), end = leaf_data_size(c->b);
    BtrfsKey prev, k;
    for (uint32_t i = 0; i < n; i++) {
        item_key(l, (int)i, &k);
        if (i && btrfs_comp_keys(&prev, &k) >= 0)
            problem(c, "leaf %llu: keys out of order at slot %u", (unsigned long long)l->start, i);
        uint32_t off = item_offset(l, (int)i), size = item_size(l, (int)i);
        if (off + size != end || off < n * ITEM_SIZE)
            problem(c, "leaf %llu: item %u data is misplaced", (unsigned long long)l->start, i);
        end = off;
        prev = k;
    }
}

static void walk(Check *c, uint64_t bytenr, uint64_t gen, int level, const BtrfsKey *parent_key, int is_root)
{
    Btrfs *b = c->b;
    ExtentBuffer *eb;
    char why[64] = "";

    if (eb_lookup(b, bytenr) == NULL) {
        uint8_t *raw = malloc(b->nodesize);
        if (raw && disk_read_bytes(b->disk, bytenr, raw, b->nodesize) == 0 &&
            verify_tree_block(raw, b->nodesize, b->fsid, bytenr, gen, level, why, sizeof why) < 0) {
            problem(c, "tree block %llu: %s", (unsigned long long)bytenr, why);
            free(raw);
            return;
        }
        free(raw);
    }
    if (read_tree_block(b, bytenr, gen, level, &eb) < 0) {
        problem(c, "tree block %llu cannot be read", (unsigned long long)bytenr);
        return;
    }
    if (!visit(c, bytenr))
        return; /* shared: its own references were counted already */

    Rec *e = rec_get(c, bytenr, 1);
    if (e) {
        e->reached = 1;
        if (e->has_item && (e->is_data || e->key_offset != (uint64_t)level))
            problem(c, "tree block %llu: extent item says level %llu", (unsigned long long)bytenr,
                    (unsigned long long)e->key_offset);
    }
    uint64_t full = e && (e->flags & BTRFS_BLOCK_FLAG_FULL_BACKREF);
    uint32_t n = eb_nritems(eb);
    BtrfsKey first;
    if (!n && !is_root)
        problem(c, "tree block %llu is empty", (unsigned long long)bytenr);
    if (n) {
        if (level == 0)
            item_key(eb, 0, &first);
        else
            node_key(eb, 0, &first);
        if (parent_key && btrfs_comp_keys(&first, parent_key) != 0)
            problem(c, "tree block %llu: first key differs from its parent's", (unsigned long long)bytenr);
    }
    if (level == 0) {
        check_leaf(c, eb);
        for (uint32_t i = 0; i < n; i++) {
            BtrfsKey k;
            item_key(eb, (int)i, &k);
            if (k.type != BTRFS_EXTENT_DATA_KEY)
                continue;
            BtrfsFileExtentItem fi;
            memset(&fi, 0, sizeof fi);
            uint32_t sz = item_size(eb, (int)i);
            memcpy(&fi, item_ptr(eb, (int)i), sz < sizeof fi ? sz : sizeof fi);
            if (fi.type == BTRFS_FILE_EXTENT_INLINE || !fi.disk_bytenr)
                continue;
            Ref r = full ? (Ref){ BTRFS_SHARED_DATA_REF_KEY, 0, bytenr, 0, 0, 1 }
                         : (Ref){ BTRFS_EXTENT_DATA_REF_KEY, eb_owner(eb), 0, k.objectid, k.offset - fi.offset, 1 };
            want(c, fi.disk_bytenr, 1, fi.disk_num_bytes, &r);
        }
        return;
    }
    if (n > max_node_ptrs(b))
        problem(c, "node %llu has too many pointers", (unsigned long long)bytenr);
    BtrfsKey prev;
    for (uint32_t i = 0; i < n; i++) {
        BtrfsKey k;
        node_key(eb, (int)i, &k);
        if (i && btrfs_comp_keys(&prev, &k) >= 0)
            problem(c, "node %llu: keys out of order", (unsigned long long)bytenr);
        prev = k;
        uint64_t child = node_blockptr(eb, (int)i), cgen = node_ptr_gen(eb, (int)i);
        if (cgen > eb_gen(eb))
            problem(c, "node %llu: child newer than its parent", (unsigned long long)bytenr);
        Ref r = full ? (Ref){ BTRFS_SHARED_BLOCK_REF_KEY, 0, bytenr, 0, 0, 1 }
                     : (Ref){ BTRFS_TREE_BLOCK_REF_KEY, eb_owner(eb), 0, 0, 0, 1 };
        want(c, child, 0, (uint64_t)(level - 1), &r);
        walk(c, child, cgen, level - 1, &k, 0);
    }
}

static void walk_all_trees(Check *c)
{
    Btrfs *b = c->b;
    Ref r = { BTRFS_TREE_BLOCK_REF_KEY, BTRFS_ROOT_TREE_OBJECTID, 0, 0, 0, 1 };
    want(c, b->tree_root->node->start, 0, (uint64_t)eb_level(b->tree_root->node), &r);
    walk(c, b->tree_root->node->start, eb_gen(b->tree_root->node), eb_level(b->tree_root->node), NULL, 1);

    Path p;
    BtrfsKey k = mkkey(0, 0, 0), f;
    int ret = btrfs_search_forward(b, b->tree_root, &k, &p);
    while (ret == 0) {
        item_key(p.nodes[0], p.slots[0], &f);
        if (f.type == BTRFS_ROOT_ITEM_KEY) {
            BtrfsRootItem ri;
            memset(&ri, 0, sizeof ri);
            uint32_t sz = item_size(p.nodes[0], p.slots[0]);
            memcpy(&ri, item_ptr(p.nodes[0], p.slots[0]), sz < sizeof ri ? sz : sizeof ri);
            r.root = f.objectid;
            want(c, ri.bytenr, 0, ri.level, &r);
            walk(c, ri.bytenr, ri.generation, ri.level, NULL, 1);
        }
        ret = btrfs_next_item(b, &p);
    }
}

static int cmp_ref(const void *x, const void *y)
{
    const Ref *a = x, *b = y;
#define CMP(f)          \
    if (a->f != b->f)   \
        return a->f < b->f ? -1 : 1;
    CMP(type) CMP(root) CMP(parent) CMP(owner) CMP(offset) CMP(count)
#undef CMP
    return 0;
}

static int cmp_rec(const void *x, const void *y)
{
    const Rec *a = *(Rec *const *)x, *b = *(Rec *const *)y;
    return a->bytenr < b->bytenr ? -1 : a->bytenr > b->bytenr;
}

static uint64_t rec_len(const Check *c, const Rec *e)
{
    return e->is_data ? e->key_offset : c->b->nodesize;
}

static void compare_refs(Check *c)
{
    for (size_t i = 0; i < c->nrecs; i++) {
        Rec *e = c->recs[i];
        unsigned long long at = (unsigned long long)e->bytenr;
        if (!e->has_item) {
            problem(c, "extent %llu is used but has no extent item", at);
            continue;
        }
        if (!e->is_data && !e->reached)
            problem(c, "tree block %llu is allocated but no tree uses it (leaked)", at);
        if (e->is_data && !e->nwant)
            problem(c, "data extent %llu is allocated but no file uses it (leaked)", at);
        uint64_t sum = 0;
        for (size_t j = 0; j < e->nhave; j++)
            sum += e->have[j].count;
        if (sum != e->refs)
            problem(c, "extent %llu: refs %llu but its back-references add up to %llu", at,
                    (unsigned long long)e->refs, (unsigned long long)sum);
        qsort(e->have, e->nhave, sizeof *e->have, cmp_ref);
        qsort(e->want, e->nwant, sizeof *e->want, cmp_ref);
        int ok = e->nhave == e->nwant;
        for (size_t j = 0; ok && j < e->nhave; j++)
            ok = same(&e->have[j], &e->want[j]) && e->have[j].count == e->want[j].count;
        if (!ok)
            problem(c, "extent %llu: back-references differ from what the trees say (%zu stored, %zu expected)",
                    at, e->nhave, e->nwant);
    }
    qsort(c->recs, c->nrecs, sizeof *c->recs, cmp_rec);
    for (size_t i = 0; i + 1 < c->nrecs; i++)
        if (c->recs[i]->has_item && c->recs[i + 1]->has_item &&
            c->recs[i]->bytenr + rec_len(c, c->recs[i]) > c->recs[i + 1]->bytenr)
            problem(c, "extents %llu and %llu overlap", (unsigned long long)c->recs[i]->bytenr,
                    (unsigned long long)c->recs[i + 1]->bytenr);
    /* recs is sorted from here on: the index is stale, rebuild it. */
    free(c->index);
    c->index = NULL;
    c->nindex = 0;
    rehash(c);
}

/* ------------------------------------- 3 and 4: block groups, free space */

static void check_block_groups(Check *c)
{
    Btrfs *b = c->b;
    uint64_t total = 0, disk_bytes = (uint64_t)disk_blocks(b->disk) * SECTORSIZE;
    size_t r = 0;

    for (size_t g = 0; g < b->nbgs; g++) {
        BlockGroup *bg = b->bgs[g];
        uint64_t used = 0, bg_end = bg->start + bg->len;
        if (bg->start + bg->len > disk_bytes || bg->start < 1024 * 1024)
            problem(c, "block group %llu is outside the usable disk", (unsigned long long)bg->start);
        if (g && b->bgs[g - 1]->start + b->bgs[g - 1]->len > bg->start)
            problem(c, "block groups %llu and %llu overlap", (unsigned long long)b->bgs[g - 1]->start,
                    (unsigned long long)bg->start);
        while (r < c->nrecs && (!c->recs[r]->has_item || c->recs[r]->bytenr < bg->start)) {
            if (c->recs[r]->has_item)
                problem(c, "extent %llu is in no block group", (unsigned long long)c->recs[r]->bytenr);
            r++;
        }

        /* The free space tree: what it says is free, merged and sorted. */
        RangeSet want_free = { 0 };
        uint64_t pos = bg->start;
        size_t first = r;
        for (; r < c->nrecs && c->recs[r]->bytenr < bg_end; r++) {
            Rec *e = c->recs[r];
            if (!e->has_item)
                continue;
            uint64_t len = rec_len(c, e);
            if (e->bytenr + len > bg_end)
                problem(c, "extent %llu crosses the end of its block group", (unsigned long long)e->bytenr);
            if (e->is_data != ((bg->flags & BTRFS_BLOCK_GROUP_DATA) != 0))
                problem(c, "extent %llu is in a block group of the wrong type", (unsigned long long)e->bytenr);
            if (is_super_range(e->bytenr, len, disk_bytes))
                problem(c, "extent %llu covers a superblock copy", (unsigned long long)e->bytenr);
            used += len;
            if (e->bytenr > pos)
                rs_add(&want_free, pos, e->bytenr - pos);
            pos = e->bytenr + len > pos ? e->bytenr + len : pos;
        }
        (void)first;
        if (pos < bg_end)
            rs_add(&want_free, pos, bg_end - pos);
        if (used != bg->used)
            problem(c, "block group %llu: used %llu, but its extents add up to %llu",
                    (unsigned long long)bg->start, (unsigned long long)bg->used, (unsigned long long)used);
        total += used;

        BtrfsKey k = mkkey(bg->start, BTRFS_FREE_SPACE_INFO_KEY, bg->len), f;
        BtrfsFreeSpaceInfo fi;
        if (btrfs_lookup(b, b->fst_root, &k, &fi, sizeof fi, NULL) < 0) {
            problem(c, "block group %llu has no FREE_SPACE_INFO", (unsigned long long)bg->start);
        } else {
            Path p;
            size_t i = 0;
            uint32_t count = 0;
            int ok = 1;
            k = mkkey(bg->start, BTRFS_FREE_SPACE_EXTENT_KEY, 0);
            int ret = btrfs_search_forward(b, b->fst_root, &k, &p);
            while (ret == 0) {
                item_key(p.nodes[0], p.slots[0], &f);
                if (f.objectid >= bg_end || f.type != BTRFS_FREE_SPACE_EXTENT_KEY)
                    break;
                if (i >= want_free.n || want_free.r[i].start != f.objectid || want_free.r[i].len != f.offset)
                    ok = 0;
                i++;
                count++;
                ret = btrfs_next_item(b, &p);
            }
            if (!ok || i != want_free.n)
                problem(c, "block group %llu: the free space tree is not the space its extents leave",
                        (unsigned long long)bg->start);
            if (count != fi.extent_count)
                problem(c, "block group %llu: FREE_SPACE_INFO counts %u extents, the tree has %u",
                        (unsigned long long)bg->start, fi.extent_count, count);
        }
        rs_free(&want_free);
    }
    for (; r < c->nrecs; r++)
        if (c->recs[r]->has_item)
            problem(c, "extent %llu is in no block group", (unsigned long long)c->recs[r]->bytenr);
    if (total != b->super.bytes_used)
        problem(c, "superblock bytes_used %llu, extents add up to %llu", (unsigned long long)b->super.bytes_used,
                (unsigned long long)total);
}

/* ------------------------------------------------- 5. the csum tree */

static void check_csums(Check *c)
{
    Btrfs *b = c->b;
    Path p;
    BtrfsKey k = mkkey(0, 0, 0), f;
    uint64_t prev_end = 0;
    RangeSet covered = { 0 };
    uint8_t *sec = malloc(SECTORSIZE);
    int ret = btrfs_search_forward(b, b->csum_root, &k, &p);

    while (sec && ret == 0) {
        item_key(p.nodes[0], p.slots[0], &f);
        uint32_t size = item_size(p.nodes[0], p.slots[0]);
        const uint8_t *sums = item_ptr(p.nodes[0], p.slots[0]);
        if (f.objectid != BTRFS_EXTENT_CSUM_OBJECTID || f.type != BTRFS_EXTENT_CSUM_KEY || size % 4 || !size) {
            problem(c, "bad item in the csum tree");
        } else {
            uint64_t end = f.offset + (uint64_t)(size / 4) * SECTORSIZE;
            if (f.offset < prev_end)
                problem(c, "csum items overlap at %llu", (unsigned long long)f.offset);
            prev_end = end;
            rs_add(&covered, f.offset, end - f.offset);
            for (uint32_t i = 0; i < size / 4; i++) {
                uint64_t at = f.offset + (uint64_t)i * SECTORSIZE;
                if (disk_read_bytes(b->disk, at, sec, SECTORSIZE) < 0 || btrfs_csum(sec, SECTORSIZE) != get32(sums + i * 4))
                    problem(c, "data at %llu does not match its checksum", (unsigned long long)at);
            }
        }
        ret = btrfs_next_item(b, &p);
    }
    /* Exactly the data extents are covered. */
    RangeSet data = { 0 };
    for (size_t i = 0; i < c->nrecs; i++)
        if (c->recs[i]->has_item && c->recs[i]->is_data)
            rs_add(&data, c->recs[i]->bytenr, c->recs[i]->key_offset);
    int same_sets = covered.n == data.n;
    for (size_t i = 0; same_sets && i < data.n; i++)
        same_sets = covered.r[i].start == data.r[i].start && covered.r[i].len == data.r[i].len;
    if (!same_sets)
        problem(c, "the csum tree does not cover exactly the data extents");
    rs_free(&covered);
    rs_free(&data);
    free(sec);
}

/* ----------------------------------------------------- 6. FS trees */

static int has_dir_entry(Btrfs *b, Root *root, uint64_t dir, const char *name, size_t len, uint64_t index,
                         const BtrfsDiskKey *loc)
{
    uint8_t buf[sizeof(BtrfsDirItem) + FS_NAME_MAX];
    BtrfsDirItem di;
    uint32_t size;
    BtrfsKey k = mkkey(dir, BTRFS_DIR_INDEX_KEY, index);
    if (btrfs_lookup(b, root, &k, buf, sizeof buf, &size) < 0)
        return 0;
    memcpy(&di, buf, sizeof di);
    if (di.name_len != len || memcmp(buf + sizeof di, name, len) || di.location.objectid != loc->objectid ||
        di.location.type != loc->type)
        return 0;
    /* And its DIR_ITEM. */
    Path p;
    k = mkkey(dir, BTRFS_DIR_ITEM_KEY, btrfs_name_hash(name, len));
    if (btrfs_search_forward(b, root, &k, &p) != 0)
        return 0;
    BtrfsKey f;
    item_key(p.nodes[0], p.slots[0], &f);
    if (btrfs_comp_keys(&f, &k))
        return 0;
    const uint8_t *d = item_ptr(p.nodes[0], p.slots[0]);
    uint32_t sz = item_size(p.nodes[0], p.slots[0]);
    for (uint32_t off = 0; off + sizeof di <= sz;) {
        memcpy(&di, d + off, sizeof di);
        if (di.name_len == len && !memcmp(d + off + sizeof di, name, len))
            return di.location.objectid == loc->objectid && di.location.type == loc->type;
        off += (uint32_t)sizeof di + di.name_len + di.data_len;
    }
    return 0;
}

typedef struct {
    uint64_t ino;
    int have_inode;
    BtrfsInodeItem ii;
    uint64_t names;        /* entries in its INODE_REFs        */
    uint64_t dir_bytes;    /* 2 * name lengths of DIR_INDEXes  */
    uint64_t dir_items, dir_indexes;
    uint64_t ext_bytes, ext_end;
    int nexts, has_inline;
} Ino;

static void finish_inode(Check *c, Root *root, Ino *in)
{
    unsigned long long id = (unsigned long long)root->objectid, ino = (unsigned long long)in->ino;
    if (!in->have_inode) {
        problem(c, "subvolume %llu: items for inode %llu, which has no INODE_ITEM", id, ino);
        return;
    }
    int dir = (in->ii.mode & 0170000u) == 0040000u;
    if (in->names != in->ii.nlink && !dir)
        problem(c, "subvolume %llu inode %llu: nlink %u but %llu names", id, ino, in->ii.nlink,
                (unsigned long long)in->names);
    if (dir) {
        if (in->ii.nlink != 1 || in->names != 1)
            problem(c, "subvolume %llu dir %llu: a directory has nlink 1 and one name", id, ino);
        if (in->ii.size != in->dir_bytes)
            problem(c, "subvolume %llu dir %llu: size %llu, its names add up to %llu", id, ino,
                    (unsigned long long)in->ii.size, (unsigned long long)in->dir_bytes);
        if (in->dir_items != in->dir_indexes)
            problem(c, "subvolume %llu dir %llu: %llu DIR_ITEM names but %llu DIR_INDEX items", id, ino,
                    (unsigned long long)in->dir_items, (unsigned long long)in->dir_indexes);
        if (in->nexts)
            problem(c, "subvolume %llu dir %llu has file extents", id, ino);
    } else {
        if (in->ext_bytes != in->ii.nbytes)
            problem(c, "subvolume %llu inode %llu: nbytes %llu, its extents add up to %llu", id, ino,
                    (unsigned long long)in->ii.nbytes, (unsigned long long)in->ext_bytes);
        uint64_t limit = (in->ii.size + SECTORSIZE - 1) / SECTORSIZE * SECTORSIZE;
        if (in->ext_end > limit)
            problem(c, "subvolume %llu inode %llu: extents go past the end of the file", id, ino);
        if (in->has_inline && (in->nexts != 1 || in->ext_end != in->ii.size))
            problem(c, "subvolume %llu inode %llu: an inline extent must be the whole file", id, ino);
    }
}

static void check_fs_tree(Check *c, Root *root)
{
    Btrfs *b = c->b;
    Path p;
    BtrfsKey k = mkkey(0, 0, 0), f;
    Ino in = { 0 };
    int have = 0;
    unsigned long long id = (unsigned long long)root->objectid;
    int ret = btrfs_search_forward(b, root, &k, &p);

    while (ret == 0) {
        ExtentBuffer *l = p.nodes[0];
        int s = p.slots[0];
        item_key(l, s, &f);
        const uint8_t *d = item_ptr(l, s);
        uint32_t size = item_size(l, s);
        if (!have || f.objectid != in.ino) {
            if (have)
                finish_inode(c, root, &in);
            memset(&in, 0, sizeof in);
            in.ino = f.objectid;
            have = 1;
        }
        switch (f.type) {
        case BTRFS_INODE_ITEM_KEY:
            memcpy(&in.ii, d, sizeof in.ii);
            in.have_inode = 1;
            break;
        case BTRFS_INODE_REF_KEY:
            for (uint32_t off = 0; off + sizeof(BtrfsInodeRef) <= size;) {
                BtrfsInodeRef ir;
                memcpy(&ir, d + off, sizeof ir);
                const char *name = (const char *)d + off + sizeof ir;
                in.names++;
                int self = f.objectid == BTRFS_FIRST_FREE_OBJECTID && f.offset == f.objectid && ir.name_len == 2 &&
                           !memcmp(name, "..", 2);
                BtrfsDiskKey loc = { f.objectid, BTRFS_INODE_ITEM_KEY, 0 };
                if (!self && !has_dir_entry(b, root, f.offset, name, ir.name_len, ir.index, &loc))
                    problem(c, "subvolume %llu inode %llu: a name with no matching DIR_ITEM / DIR_INDEX", id,
                            (unsigned long long)f.objectid);
                off += (uint32_t)sizeof ir + ir.name_len;
            }
            break;
        case BTRFS_DIR_ITEM_KEY:
            for (uint32_t off = 0; off + sizeof(BtrfsDirItem) <= size;) {
                BtrfsDirItem di;
                memcpy(&di, d + off, sizeof di);
                if (btrfs_name_hash((const char *)d + off + sizeof di, di.name_len) != f.offset)
                    problem(c, "subvolume %llu dir %llu: a DIR_ITEM under the wrong hash", id,
                            (unsigned long long)f.objectid);
                in.dir_items++;
                off += (uint32_t)sizeof di + di.name_len + di.data_len;
            }
            break;
        case BTRFS_DIR_INDEX_KEY: {
            BtrfsDirItem di;
            memcpy(&di, d, sizeof di);
            in.dir_indexes++;
            in.dir_bytes += 2u * di.name_len;
            const char *name = (const char *)d + sizeof di;
            if (di.location.type == BTRFS_INODE_ITEM_KEY) {
                /* The target must name us back. */
                BtrfsKey rk = mkkey(di.location.objectid, BTRFS_INODE_REF_KEY, f.objectid);
                uint8_t buf[4096];
                uint32_t rs;
                int found = 0;
                if (btrfs_lookup(b, root, &rk, buf, sizeof buf, &rs) == 0)
                    for (uint32_t off = 0; off + sizeof(BtrfsInodeRef) <= rs && off < sizeof buf;) {
                        BtrfsInodeRef ir;
                        memcpy(&ir, buf + off, sizeof ir);
                        if (ir.index == f.offset && ir.name_len == di.name_len &&
                            !memcmp(buf + off + sizeof ir, name, di.name_len))
                            found = 1;
                        off += (uint32_t)sizeof ir + ir.name_len;
                    }
                if (!found)
                    problem(c, "subvolume %llu dir %llu: entry %llu's inode does not name it", id,
                            (unsigned long long)f.objectid, (unsigned long long)f.offset);
            } else if (di.location.type == BTRFS_ROOT_ITEM_KEY) {
                BtrfsKey rk = mkkey(root->objectid, BTRFS_ROOT_REF_KEY, di.location.objectid);
                uint8_t buf[sizeof(BtrfsRootRef) + FS_NAME_MAX];
                BtrfsRootRef rr;
                BtrfsKey bk = mkkey(di.location.objectid, BTRFS_ROOT_BACKREF_KEY, root->objectid);
                int own = btrfs_lookup(b, b->tree_root, &bk, NULL, 0, NULL) == 0;
                if (btrfs_lookup(b, b->tree_root, &rk, buf, sizeof buf, NULL) < 0) {
                    if (own) /* without a backref, it was copied by a snapshot: fine */
                        problem(c, "subvolume %llu: entry for subvolume %llu has no ROOT_REF", id,
                                (unsigned long long)di.location.objectid);
                } else {
                    memcpy(&rr, buf, sizeof rr);
                    if (rr.dirid != f.objectid || rr.sequence != f.offset || rr.name_len != di.name_len ||
                        memcmp(buf + sizeof rr, name, di.name_len))
                        problem(c, "subvolume %llu: ROOT_REF of %llu does not match its entry", id,
                                (unsigned long long)di.location.objectid);
                }
            } else {
                problem(c, "subvolume %llu: a directory entry with a bad location", id);
            }
            break;
        }
        case BTRFS_EXTENT_DATA_KEY: {
            BtrfsFileExtentItem fi;
            memset(&fi, 0, sizeof fi);
            memcpy(&fi, d, size < sizeof fi ? size : sizeof fi);
            if (f.offset < in.ext_end)
                problem(c, "subvolume %llu inode %llu: file extents overlap", id, (unsigned long long)f.objectid);
            in.nexts++;
            if (fi.type == BTRFS_FILE_EXTENT_INLINE) {
                in.has_inline = 1;
                if (f.offset != 0 || size - BTRFS_FILE_EXTENT_INLINE_DATA_START != fi.ram_bytes)
                    problem(c, "subvolume %llu inode %llu: bad inline extent", id, (unsigned long long)f.objectid);
                in.ext_bytes += fi.ram_bytes;
                in.ext_end = f.offset + fi.ram_bytes;
            } else {
                if (fi.type != BTRFS_FILE_EXTENT_REG || fi.offset + fi.num_bytes > fi.disk_num_bytes ||
                    !fi.num_bytes || f.offset % SECTORSIZE || fi.num_bytes % SECTORSIZE)
                    problem(c, "subvolume %llu inode %llu: bad file extent at %llu", id,
                            (unsigned long long)f.objectid, (unsigned long long)f.offset);
                in.ext_bytes += fi.num_bytes;
                in.ext_end = f.offset + fi.num_bytes;
            }
            break;
        }
        default:
            break;
        }
        ret = btrfs_next_item(b, &p);
    }
    if (have)
        finish_inode(c, root, &in);
    BtrfsInodeItem ri;
    if (inode_get(b, (Inode){ root, BTRFS_FIRST_FREE_OBJECTID }, &ri) < 0 || (ri.mode & 0170000u) != 0040000u)
        problem(c, "subvolume %llu has no root directory", id);
}

static void check_subvolumes(Check *c)
{
    Btrfs *b = c->b;
    Path p;
    BtrfsKey k = mkkey(0, 0, 0), f;
    uint64_t ids[4096];
    size_t n = 0;
    int ret = btrfs_search_forward(b, b->tree_root, &k, &p);
    while (ret == 0 && n < sizeof ids / sizeof *ids) {
        item_key(p.nodes[0], p.slots[0], &f);
        if (f.type == BTRFS_ROOT_ITEM_KEY &&
            (f.objectid == BTRFS_FS_TREE_OBJECTID ||
             (f.objectid >= BTRFS_FIRST_FREE_OBJECTID && f.objectid <= BTRFS_LAST_FREE_OBJECTID)))
            ids[n++] = f.objectid;
        ret = btrfs_next_item(b, &p);
    }
    for (size_t i = 0; i < n; i++) {
        Root *r = get_root(b, ids[i]);
        if (!r)
            continue;
        unsigned long long id = (unsigned long long)ids[i];
        BtrfsKey ok = mkkey(BTRFS_ORPHAN_OBJECTID, BTRFS_ORPHAN_ITEM_KEY, ids[i]);
        int orphan = btrfs_lookup(b, b->tree_root, &ok, NULL, 0, NULL) == 0;
        if (r->item.refs == 0) {
            if (!orphan)
                problem(c, "subvolume %llu is deleted but has no orphan item", id);
            continue; /* waiting for the cleaner */
        }
        if (orphan)
            problem(c, "subvolume %llu is live but has an orphan item", id);
        check_fs_tree(c, r);
        /* Its name and the uuid tree. */
        uint64_t parent, dirid;
        char name[FS_NAME_MAX + 1];
        if (ids[i] != BTRFS_FS_TREE_OBJECTID) {
            if (find_root_ref(b, ids[i], &parent, &dirid, name) < 0) {
                problem(c, "subvolume %llu has no ROOT_BACKREF", id);
            } else {
                BtrfsKey rk = mkkey(parent, BTRFS_ROOT_REF_KEY, ids[i]);
                if (btrfs_lookup(b, b->tree_root, &rk, NULL, 0, NULL) < 0)
                    problem(c, "subvolume %llu has no ROOT_REF in its parent", id);
            }
        }
        BtrfsKey uk = mkkey(get64(r->item.uuid), BTRFS_UUID_KEY_SUBVOL, get64(r->item.uuid + 8));
        uint8_t ub[4096];
        uint32_t us;
        int found = 0;
        if (btrfs_lookup(b, b->uuid_root, &uk, ub, sizeof ub, &us) == 0)
            for (uint32_t off = 0; off + 8 <= us && off < sizeof ub; off += 8)
                found |= get64(ub + off) == ids[i];
        if (!found)
            problem(c, "subvolume %llu is missing from the uuid tree", id);
    }
}

/* ----------------------------------------------------------------- run */

int btrfs_check(Disk *disk, int verbose)
{
    BtrfsMountOpts o = { 0, 1 };
    Btrfs *b;
    Check c;
    memset(&c, 0, sizeof c);
    c.verbose = verbose;

    int r = btrfs_mount(disk, &o, &b);
    if (r < 0) {
        if (verbose)
            printf("  problem: cannot mount (%d)\n", r);
        return 1;
    }
    c.b = b;
    scan_extent_tree(&c);
    walk_all_trees(&c);
    compare_refs(&c);
    check_block_groups(&c);
    check_csums(&c);
    check_subvolumes(&c);

    for (size_t i = 0; i < c.nrecs; i++) {
        free(c.recs[i]->have);
        free(c.recs[i]->want);
        free(c.recs[i]);
    }
    free(c.recs);
    free(c.index);
    free(c.visited);
    free(c.vindex);
    btrfs_crash(b);
    return c.errors;
}
