/*
 * disk_io.c - superblocks, mkfs, mount (fs/btrfs/disk-io.c, and mkfs /
 * rescue from btrfs-progs).
 *
 * The only fixed places on the disk are the superblock copies, at 64 KiB,
 * 64 MiB and 256 GiB (those that fit). Every commit writes the same
 * superblock to all of them; each copy records its own bytenr and a
 * crc32c. The superblock names the root tree's root block; the root tree
 * names every other tree. It also keeps four BACKUP ROOTS, the roots of
 * the last four commits, which mount -o usebackuproot falls back on when
 * the newest root tree cannot be read.
 *
 * Like the kernel, mount reads only the primary copy. If it is damaged,
 * btrfs rescue super-recover (btrfs_super_recover) copies the newest
 * good mirror over it.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "ctree.h"

#define MiB (1024ULL * 1024)

static uint64_t disk_bytes(Disk *d)
{
    return (uint64_t)disk_blocks(d) * SECTORSIZE;
}

static void super_csum(BtrfsSuper *sb)
{
    uint8_t *p = (uint8_t *)sb;
    uint32_t c = btrfs_csum(p + BTRFS_CSUM_SIZE, BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE);
    memset(sb->csum, 0, BTRFS_CSUM_SIZE);
    put32(sb->csum, c);
}

/* 0 = good; -ENOENT = no such copy on this disk; -EINVAL = no btrfs
 * there; -EIO = damaged. */
static int read_super_mirror(Disk *d, int m, BtrfsSuper *sb)
{
    uint64_t off = super_mirror_offset(m);
    if (off + BTRFS_SUPER_INFO_SIZE > disk_bytes(d))
        return -ENOENT;
    TRY(disk_read_bytes(d, off, sb, BTRFS_SUPER_INFO_SIZE));
    if (sb->magic != BTRFS_MAGIC)
        return -EINVAL;
    uint8_t *p = (uint8_t *)sb;
    if (get32(sb->csum) != btrfs_csum(p + BTRFS_CSUM_SIZE, BTRFS_SUPER_INFO_SIZE - BTRFS_CSUM_SIZE))
        return -EIO;
    return sb->bytenr == off ? 0 : -EIO;
}

uint64_t btrfs_disk_generation(Disk *disk)
{
    BtrfsSuper sb;
    return read_super_mirror(disk, 0, &sb) == 0 ? sb.generation : 0;
}

int write_all_supers(Btrfs *b)
{
    BtrfsSuper copy;
    for (int m = 0; m < BTRFS_SUPER_MIRROR_MAX; m++) {
        uint64_t off = super_mirror_offset(m);
        if (off + BTRFS_SUPER_INFO_SIZE > disk_bytes(b->disk))
            break;
        copy = b->super;
        copy.bytenr = off;
        super_csum(&copy);
        TRY(disk_write_bytes(b->disk, off, &copy, BTRFS_SUPER_INFO_SIZE));
    }
    return disk_flush(b->disk);
}

typedef struct {
    uint64_t bytenr, gen;
    uint8_t level;
} RootPos;

static RootPos root_pos(Root *r)
{
    RootPos p = { 0, 0, 0 };
    if (r && r->node) {
        p.bytenr = r->node->start;
        p.gen = eb_gen(r->node);
        p.level = (uint8_t)eb_level(r->node);
    }
    return p;
}

/* backup_super_roots: this commit's roots go in the next backup slot. */
void backup_super_roots(Btrfs *b)
{
    BtrfsRootBackup *bk = &b->super.super_roots[b->backup_index];
    RootPos t = root_pos(b->tree_root), e = root_pos(b->extent_root);
    RootPos f = root_pos(get_root(b, BTRFS_FS_TREE_OBJECTID)), c = root_pos(b->csum_root);
    memset(bk, 0, sizeof *bk);
    bk->tree_root = t.bytenr;
    bk->tree_root_gen = t.gen;
    bk->tree_root_level = t.level;
    bk->extent_root = e.bytenr;
    bk->extent_root_gen = e.gen;
    bk->extent_root_level = e.level;
    bk->fs_root = f.bytenr;
    bk->fs_root_gen = f.gen;
    bk->fs_root_level = f.level;
    bk->csum_root = c.bytenr;
    bk->csum_root_gen = c.gen;
    bk->csum_root_level = c.level;
    bk->total_bytes = b->super.total_bytes;
    bk->bytes_used = b->super.bytes_used;
    bk->num_devices = 1;
    b->backup_index = (b->backup_index + 1) % BTRFS_NUM_BACKUP_ROOTS;
}

/* ------------------------------------------------------------- fs_info */

static Btrfs *fs_info_new(Disk *disk, uint32_t nodesize)
{
    Btrfs *b = calloc(1, sizeof *b);
    if (b) {
        b->disk = disk;
        b->nodesize = nodesize;
    }
    return b;
}

static void fs_info_free(Btrfs *b)
{
    if (!b)
        return;
    roots_destroy(b);
    eb_cache_destroy(b);
    block_groups_destroy(b);
    delayed_refs_destroy(b);
    while (b->pending_snaps) {
        PendingSnap *next = b->pending_snaps->next;
        free(b->pending_snaps);
        b->pending_snaps = next;
    }
    free(b);
}

static int valid_nodesize(uint32_t n)
{
    return n >= SECTORSIZE && n <= 65536 && (n & (n - 1)) == 0;
}

/* ---------------------------------------------------------------- mkfs */

int btrfs_mkfs(Disk *disk, const BtrfsMkfsOpts *opts)
{
    uint32_t ns = opts && opts->nodesize ? opts->nodesize : 16384;
    if (!valid_nodesize(ns))
        return -EINVAL;
    if (disk_bytes(disk) < 4 * MiB)
        return -ENOSPC;

    /* Wipe any old superblocks first. */
    uint8_t *zero = calloc(1, BTRFS_SUPER_INFO_SIZE);
    if (!zero)
        return -ENOMEM;
    for (int m = 0; m < BTRFS_SUPER_MIRROR_MAX; m++) {
        uint64_t off = super_mirror_offset(m);
        if (off + BTRFS_SUPER_INFO_SIZE <= disk_bytes(disk))
            disk_write_bytes(disk, off, zero, BTRFS_SUPER_INFO_SIZE);
    }
    free(zero);

    Btrfs *b = fs_info_new(disk, ns);
    if (!b)
        return -ENOMEM;
    btrfs_random_uuid(b->fsid);
    b->generation = 0;
    b->transid = 1;
    b->no_flush = 1;
    b->free_root_objectid = BTRFS_FIRST_FREE_OBJECTID;

    BtrfsSuper *sb = &b->super;
    memcpy(sb->fsid, b->fsid, BTRFS_FSID_SIZE);
    memcpy(sb->metadata_uuid, b->fsid, BTRFS_FSID_SIZE);
    sb->magic = BTRFS_MAGIC;
    sb->total_bytes = disk_bytes(disk);
    sb->root_dir_objectid = BTRFS_ROOT_TREE_DIR_OBJECTID;
    sb->num_devices = 1;
    sb->sectorsize = SECTORSIZE;
    sb->nodesize = ns;
    sb->unused_leafsize = ns;
    sb->stripesize = SECTORSIZE;
    sb->csum_type = BTRFS_CSUM_TYPE_CRC32;
    sb->compat_ro_flags = BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE | BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID;
    sb->incompat_flags = BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF | BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA |
                         BTRFS_FEATURE_INCOMPAT_NO_HOLES;
    if (opts && opts->label)
        snprintf(sb->label, sizeof sb->label, "%s", opts->label);
    sb->dev_item.devid = 1;
    sb->dev_item.total_bytes = disk_bytes(disk);
    sb->dev_item.io_align = sb->dev_item.io_width = sb->dev_item.sector_size = SECTORSIZE;
    btrfs_random_uuid(sb->dev_item.uuid);
    memcpy(sb->dev_item.fsid, b->fsid, BTRFS_FSID_SIZE);

    Trans t = { b, 0 };
    int r = alloc_chunk(b, BTRFS_BLOCK_GROUP_METADATA);
    if (!r)
        r = alloc_chunk(b, BTRFS_BLOCK_GROUP_DATA);
    if (!r) {
        b->tree_root = root_new(b, BTRFS_ROOT_TREE_OBJECTID);
        r = b->tree_root ? alloc_tree_block(&t, b->tree_root, 0, &b->tree_root->node) : -ENOMEM;
    }
    if (!r)
        r = create_empty_tree(&t, BTRFS_EXTENT_TREE_OBJECTID, &b->extent_root);
    if (!r)
        r = create_empty_tree(&t, BTRFS_CSUM_TREE_OBJECTID, &b->csum_root);
    if (!r)
        r = create_empty_tree(&t, BTRFS_FREE_SPACE_TREE_OBJECTID, &b->fst_root);
    if (!r)
        r = create_empty_tree(&t, BTRFS_UUID_TREE_OBJECTID, &b->uuid_root);

    /* The top-level subvolume, 5, with an empty "/". */
    Root *fs = NULL;
    if (!r) {
        r = create_empty_tree(&t, BTRFS_FS_TREE_OBJECTID, &fs);
    }
    if (!r) {
        init_subvol_item(b, &fs->item);
        fs->dirty = 1;
        r = make_root_dir(&t, fs, BTRFS_FIRST_FREE_OBJECTID);
    }
    if (!r)
        r = uuid_tree_add(&t, fs->item.uuid, BTRFS_FS_TREE_OBJECTID);

    /* The root tree's directory, holding "default" -> subvolume 5. */
    if (!r)
        r = make_root_dir(&t, b->tree_root, BTRFS_ROOT_TREE_DIR_OBJECTID);
    if (!r) {
        uint8_t entry[sizeof(BtrfsDirItem) + 7];
        BtrfsDirItem di = { { BTRFS_FS_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, (uint64_t)-1 }, b->transid, 0, 7,
                            BTRFS_FT_DIR };
        memcpy(entry, &di, sizeof di);
        memcpy(entry + sizeof di, "default", 7);
        BtrfsKey k = mkkey(BTRFS_ROOT_TREE_DIR_OBJECTID, BTRFS_DIR_ITEM_KEY, btrfs_name_hash("default", 7));
        r = btrfs_insert_item(&t, b->tree_root, &k, entry, sizeof entry);
    }
    if (!r)
        r = btrfs_commit(b);
    fs_info_free(b);
    return r;
}

/* --------------------------------------------------------------- mount */

static uint64_t count_tree_bytes(Btrfs *b, ExtentBuffer *eb)
{
    uint64_t n = b->nodesize;
    if (eb_level(eb) > 0)
        for (int i = 0; i < (int)eb_nritems(eb); i++) {
            ExtentBuffer *c;
            if (read_tree_block(b, node_blockptr(eb, i), node_ptr_gen(eb, i), eb_level(eb) - 1, &c) == 0)
                n += count_tree_bytes(b, c);
        }
    return n;
}

static int open_trees(Btrfs *b, uint64_t root_bytenr, uint64_t gen, int level)
{
    b->tree_root = root_new(b, BTRFS_ROOT_TREE_OBJECTID);
    if (!b->tree_root)
        return -ENOMEM;
    TRY(read_tree_block(b, root_bytenr, gen, level, &b->tree_root->node));
    b->tree_root->commit_root = root_bytenr;
    b->extent_root = get_root(b, BTRFS_EXTENT_TREE_OBJECTID);
    b->csum_root = get_root(b, BTRFS_CSUM_TREE_OBJECTID);
    b->fst_root = get_root(b, BTRFS_FREE_SPACE_TREE_OBJECTID);
    b->uuid_root = get_root(b, BTRFS_UUID_TREE_OBJECTID);
    if (!b->extent_root || !b->csum_root || !b->fst_root || !b->uuid_root ||
        !get_fs_root(b, BTRFS_FS_TREE_OBJECTID))
        return -EIO;
    b->tree_root->item.bytes_used = count_tree_bytes(b, b->tree_root->node);
    TRY(load_block_groups(b));
    b->free_root_objectid = highest_root_objectid(b) + 1;
    return 0;
}

typedef struct {
    uint64_t bytenr, gen;
    int level;
} RootTry;

int btrfs_mount(Disk *disk, const BtrfsMountOpts *opts, Btrfs **out)
{
    BtrfsSuper sb;
    int r = read_super_mirror(disk, 0, &sb);
    if (r == -EINVAL || r == -ENOENT)
        return -ENOENT;
    if (r < 0)
        return -EIO;
    if (sb.sectorsize != SECTORSIZE || !valid_nodesize(sb.nodesize) || sb.csum_type != BTRFS_CSUM_TYPE_CRC32 ||
        sb.total_bytes > disk_bytes(disk) ||
        (sb.incompat_flags & ~(BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF | BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA |
                               BTRFS_FEATURE_INCOMPAT_NO_HOLES)) ||
        !(sb.compat_ro_flags & BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID))
        return -EINVAL;

    /* Attempt 0: the superblock's roots. With usebackuproot, then the
     * backups, newest first. */
    RootTry tries[1 + BTRFS_NUM_BACKUP_ROOTS];
    int ntries = 0, newest = -1;
    tries[ntries++] = (RootTry){ sb.root, sb.generation, sb.root_level };
    for (int i = 0; i < BTRFS_NUM_BACKUP_ROOTS; i++)
        if (newest < 0 || sb.super_roots[i].tree_root_gen > sb.super_roots[newest].tree_root_gen)
            newest = i;
    if (opts && opts->usebackuproot)
        for (int k = 0; k < BTRFS_NUM_BACKUP_ROOTS; k++) {
            const BtrfsRootBackup *bk = &sb.super_roots[(newest - k + 2 * BTRFS_NUM_BACKUP_ROOTS) % BTRFS_NUM_BACKUP_ROOTS];
            if (bk->tree_root && bk->tree_root_gen <= sb.generation)
                tries[ntries++] = (RootTry){ bk->tree_root, bk->tree_root_gen, bk->tree_root_level };
        }

    for (int i = 0; i < ntries; i++) {
        Btrfs *b = fs_info_new(disk, sb.nodesize);
        if (!b)
            return -ENOMEM;
        b->super = sb;
        memcpy(b->fsid, sb.fsid, BTRFS_FSID_SIZE);
        b->generation = tries[i].gen;
        b->transid = tries[i].gen + 1;
        b->read_only = opts && opts->read_only;
        b->backup_index = (newest + 1) % BTRFS_NUM_BACKUP_ROOTS;
        if (open_trees(b, tries[i].bytenr, tries[i].gen, tries[i].level) == 0) {
            b->super.generation = tries[i].gen;
            if (i > 0)
                b->trans_dirty = 1; /* a backup root: the next commit rewrites the superblock */
            refill_global_rsv(b);
            r = drop_dead_roots(b);
            if (r < 0) {
                fs_info_free(b);
                return r;
            }
            *out = b;
            return 0;
        }
        fs_info_free(b);
    }
    return -EIO;
}

int btrfs_unmount(Btrfs *b)
{
    int r = writable(b) == 0 ? btrfs_commit(b) : 0;
    fs_info_free(b);
    return r;
}

void btrfs_crash(Btrfs *b)
{
    fs_info_free(b);
}

uint64_t btrfs_generation(const Btrfs *b) { return b->generation; }
int btrfs_is_read_only(const Btrfs *b) { return writable(b) < 0; }
Disk *btrfs_disk(Btrfs *b) { return b->disk; }
void btrfs_stats(Btrfs *b, BtrfsStats *out) { *out = b->stats; }

/* -------------------------------------------------------- super-recover */

int btrfs_super_recover(Disk *disk)
{
    BtrfsSuper sb, best;
    int have = 0, primary_ok = 0;
    uint64_t primary_gen = 0;

    for (int m = 0; m < BTRFS_SUPER_MIRROR_MAX; m++) {
        int r = read_super_mirror(disk, m, &sb);
        if (r == -ENOENT)
            break;
        if (r < 0)
            continue;
        /* A copy is good only if its root tree can be read. */
        uint8_t *node = malloc(sb.nodesize);
        if (!node)
            return -ENOMEM;
        int ok = valid_nodesize(sb.nodesize) && disk_read_bytes(disk, sb.root, node, sb.nodesize) == 0 &&
                 verify_tree_block(node, sb.nodesize, sb.fsid, sb.root, sb.generation, sb.root_level, NULL, 0) == 0;
        free(node);
        if (!ok)
            continue;
        if (m == 0) {
            primary_ok = 1;
            primary_gen = sb.generation;
        }
        if (!have || sb.generation > best.generation) {
            best = sb;
            have = 1;
        }
    }
    if (!have)
        return -ENOENT;
    if (primary_ok && primary_gen == best.generation)
        return 0;
    for (int m = 0; m < BTRFS_SUPER_MIRROR_MAX; m++) {
        uint64_t off = super_mirror_offset(m);
        if (off + BTRFS_SUPER_INFO_SIZE > disk_bytes(disk))
            break;
        sb = best;
        sb.bytenr = off;
        super_csum(&sb);
        TRY(disk_write_bytes(disk, off, &sb, BTRFS_SUPER_INFO_SIZE));
    }
    TRY(disk_flush(disk));
    return 1;
}

/* ------------------------------------------------- space and tree search */

int fs_space(Btrfs *b, FsSpace *out)
{
    memset(out, 0, sizeof *out);
    out->total_bytes = disk_bytes(b->disk);
    out->bytes_used = total_bytes_used(b);
    uint64_t allocated = 0;
    for (size_t i = 0; i < b->nbgs; i++) {
        BlockGroup *bg = b->bgs[i];
        allocated += bg->len;
        if (bg->flags & BTRFS_BLOCK_GROUP_DATA) {
            out->data_total += bg->len;
            out->data_used += bg->used + bg->reserved;
        } else {
            out->metadata_total += bg->len;
            out->metadata_used += bg->used + bg->reserved;
        }
    }
    out->block_groups = (uint32_t)b->nbgs;
    uint64_t usable = out->total_bytes > MiB ? out->total_bytes - MiB : 0;
    out->unallocated = usable > allocated ? usable - allocated : 0;
    out->free_estimate = out->data_total - out->data_used + out->unallocated;
    out->sectorsize = SECTORSIZE;
    out->nodesize = b->nodesize;
    return 0;
}

int btrfs_tree_search(Btrfs *b, uint64_t tree_id, const BtrfsKey *min, const BtrfsKey *max, fs_item_fn fn,
                      void *arg)
{
    Root *root = tree_id == BTRFS_ROOT_TREE_OBJECTID ? b->tree_root : get_root(b, tree_id);
    Path p;
    if (!root)
        return -ENOENT;
    int r = btrfs_search_forward(b, root, min, &p);
    while (r == 0) {
        BtrfsKey k;
        item_key(p.nodes[0], p.slots[0], &k);
        if (btrfs_comp_keys(&k, max) > 0)
            break;
        if (fn(arg, &k, item_ptr(p.nodes[0], p.slots[0]), item_size(p.nodes[0], p.slots[0])))
            return 0;
        r = btrfs_next_item(b, &p);
    }
    return r < 0 ? r : 0;
}
