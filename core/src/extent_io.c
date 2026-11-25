/*
 * extent_io.c - tree blocks in memory ("extent buffers").
 *
 * Every tree block read or created is kept in a cache keyed by its bytenr.
 * Blocks changed in the running transaction are marked dirty and written
 * at commit, each with a fresh checksum. A block read from the disk is
 * verified first, as btrfs does (btree_read_extent_buffer_pages):
 *
 *   - the crc32c in its header matches its contents;
 *   - its header says it is at this bytenr, in this filesystem (fsid);
 *   - its generation is the one its parent pointer expects (this catches
 *     a write that never reached the disk, or reached the wrong place);
 *   - its level is the one expected.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "ctree.h"

/* ------------------------------------------------------------ fields */

void read_disk_key(const uint8_t *p, BtrfsKey *k)
{
    k->objectid = get64(p);
    k->type = p[8];
    k->offset = get64(p + 9);
}

void write_disk_key(uint8_t *p, const BtrfsKey *k)
{
    put64(p, k->objectid);
    p[8] = k->type;
    put64(p + 9, k->offset);
}

static uint8_t *item_at(const ExtentBuffer *eb, int slot)
{
    return eb->data + HDR_SIZE + (size_t)slot * ITEM_SIZE;
}

static uint8_t *ptr_at(const ExtentBuffer *eb, int slot)
{
    return eb->data + HDR_SIZE + (size_t)slot * KEYPTR_SIZE;
}

void item_key(const ExtentBuffer *eb, int slot, BtrfsKey *k) { read_disk_key(item_at(eb, slot), k); }
uint32_t item_offset(const ExtentBuffer *eb, int slot) { return get32(item_at(eb, slot) + 17); }
uint32_t item_size(const ExtentBuffer *eb, int slot) { return get32(item_at(eb, slot) + 21); }
uint8_t *item_ptr(const ExtentBuffer *eb, int slot) { return eb->data + HDR_SIZE + item_offset(eb, slot); }
void set_item_key(ExtentBuffer *eb, int slot, const BtrfsKey *k) { write_disk_key(item_at(eb, slot), k); }
void set_item_offset(ExtentBuffer *eb, int slot, uint32_t v) { put32(item_at(eb, slot) + 17, v); }
void set_item_size(ExtentBuffer *eb, int slot, uint32_t v) { put32(item_at(eb, slot) + 21, v); }

void node_key(const ExtentBuffer *eb, int slot, BtrfsKey *k) { read_disk_key(ptr_at(eb, slot), k); }
uint64_t node_blockptr(const ExtentBuffer *eb, int slot) { return get64(ptr_at(eb, slot) + 17); }
uint64_t node_ptr_gen(const ExtentBuffer *eb, int slot) { return get64(ptr_at(eb, slot) + 25); }
void set_node_key(ExtentBuffer *eb, int slot, const BtrfsKey *k) { write_disk_key(ptr_at(eb, slot), k); }
void set_node_blockptr(ExtentBuffer *eb, int slot, uint64_t v) { put64(ptr_at(eb, slot) + 17, v); }
void set_node_ptr_gen(ExtentBuffer *eb, int slot, uint64_t v) { put64(ptr_at(eb, slot) + 25, v); }

/* --------------------------------------------------------- raw disk I/O */

int disk_read_bytes(Disk *d, uint64_t bytenr, void *buf, uint64_t len)
{
    uint8_t *p = buf;
    if (bytenr % SECTORSIZE || len % SECTORSIZE)
        return -EINVAL;
    for (uint64_t off = 0; off < len; off += SECTORSIZE) {
        uint64_t sector = (bytenr + off) / SECTORSIZE;
        if (sector >= disk_blocks(d))
            return -EIO;
        TRY(disk_read(d, (uint32_t)sector, p + off));
    }
    return 0;
}

int disk_write_bytes(Disk *d, uint64_t bytenr, const void *buf, uint64_t len)
{
    const uint8_t *p = buf;
    if (bytenr % SECTORSIZE || len % SECTORSIZE)
        return -EINVAL;
    for (uint64_t off = 0; off < len; off += SECTORSIZE) {
        uint64_t sector = (bytenr + off) / SECTORSIZE;
        if (sector >= disk_blocks(d))
            return -EIO;
        TRY(disk_write(d, (uint32_t)sector, p + off));
    }
    return 0;
}

/* -------------------------------------------------------- checksums */

void csum_tree_block(uint8_t *data, uint32_t nodesize)
{
    uint32_t c = btrfs_csum(data + BTRFS_CSUM_SIZE, nodesize - BTRFS_CSUM_SIZE);
    memset(data, 0, BTRFS_CSUM_SIZE);
    put32(data, c);
}

int verify_tree_block(const uint8_t *data, uint32_t nodesize, const uint8_t *fsid, uint64_t bytenr,
                      uint64_t gen, int level, char *why, size_t whylen)
{
    uint32_t c = btrfs_csum(data + BTRFS_CSUM_SIZE, nodesize - BTRFS_CSUM_SIZE);
    const char *err = NULL;

    if (get32(data) != c)
        err = "checksum mismatch";
    else if (memcmp(data + HDR_FSID, fsid, BTRFS_FSID_SIZE) != 0)
        err = "fsid mismatch";
    else if (get64(data + HDR_BYTENR) != bytenr)
        err = "bytenr mismatch";
    else if (gen && get64(data + HDR_GEN) != gen)
        err = "parent transid verify failed";
    else if (level >= 0 && data[HDR_LEVEL] != level)
        err = "level mismatch";
    else if (data[HDR_LEVEL] >= BTRFS_MAX_LEVEL)
        err = "bad level";
    if (err && why)
        snprintf(why, whylen, "%s", err);
    return err ? -EIO : 0;
}

/* ---------------------------------------------------------------- cache */

static size_t hash_of(const Btrfs *b, uint64_t bytenr)
{
    return (size_t)((bytenr / b->nodesize) * 2654435761u) & (b->ebhash_n - 1);
}

static int cache_grow(Btrfs *b)
{
    size_t n = b->ebhash_n ? b->ebhash_n * 2 : 1024;
    ExtentBuffer **nh = calloc(n, sizeof *nh);
    if (!nh)
        return -ENOMEM;
    size_t old_n = b->ebhash_n;
    ExtentBuffer **old = b->ebhash;
    b->ebhash = nh;
    b->ebhash_n = n;
    for (size_t i = 0; i < old_n; i++)
        for (ExtentBuffer *e = old[i], *next; e; e = next) {
            next = e->hnext;
            size_t h = hash_of(b, e->start);
            e->hnext = nh[h];
            nh[h] = e;
        }
    free(old);
    return 0;
}

ExtentBuffer *eb_lookup(Btrfs *b, uint64_t bytenr)
{
    if (!b->ebhash_n)
        return NULL;
    for (ExtentBuffer *e = b->ebhash[hash_of(b, bytenr)]; e; e = e->hnext)
        if (e->start == bytenr)
            return e;
    return NULL;
}

static int push(ExtentBuffer ***arr, size_t *n, size_t *cap, ExtentBuffer *eb)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        ExtentBuffer **na = realloc(*arr, nc * sizeof *na);
        if (!na)
            return -ENOMEM;
        *arr = na;
        *cap = nc;
    }
    (*arr)[(*n)++] = eb;
    return 0;
}

/* Take a buffer out of the cache. Its memory lives on until the commit
 * ends, because paths may still point at it. */
void eb_forget(Btrfs *b, ExtentBuffer *eb)
{
    if (eb->stale)
        return;
    ExtentBuffer **pp = &b->ebhash[hash_of(b, eb->start)];
    while (*pp && *pp != eb)
        pp = &(*pp)->hnext;
    if (*pp) {
        *pp = eb->hnext;
        b->eb_count--;
    }
    eb->stale = 1;
    eb->hnext = NULL;
    if (push(&b->stale_ebs, &b->nstale, &b->cap_stale, eb) < 0) {
        /* Out of memory: leak it rather than free memory a path may use. */
    }
}

static ExtentBuffer *eb_insert(Btrfs *b, uint64_t bytenr)
{
    if (b->eb_count + 1 > b->ebhash_n && cache_grow(b) < 0)
        return NULL;
    ExtentBuffer *old = eb_lookup(b, bytenr);
    if (old)
        eb_forget(b, old);
    ExtentBuffer *eb = calloc(1, sizeof *eb);
    uint8_t *data = calloc(1, b->nodesize);
    if (!eb || !data) {
        free(eb);
        free(data);
        return NULL;
    }
    eb->start = bytenr;
    eb->data = data;
    size_t h = hash_of(b, bytenr);
    eb->hnext = b->ebhash[h];
    b->ebhash[h] = eb;
    b->eb_count++;
    return eb;
}

ExtentBuffer *eb_new(Btrfs *b, uint64_t bytenr)
{
    return eb_insert(b, bytenr);
}

int read_tree_block(Btrfs *b, uint64_t bytenr, uint64_t gen, int level, ExtentBuffer **out)
{
    ExtentBuffer *eb = eb_lookup(b, bytenr);
    if (eb) {
        if ((gen && eb_gen(eb) != gen) || (level >= 0 && eb_level(eb) != level))
            return -EIO; /* the parent points at a different version */
        *out = eb;
        return 0;
    }
    eb = eb_insert(b, bytenr);
    if (!eb)
        return -ENOMEM;
    int r = disk_read_bytes(b->disk, bytenr, eb->data, b->nodesize);
    if (r == 0)
        r = verify_tree_block(eb->data, b->nodesize, b->fsid, bytenr, gen, level, NULL, 0);
    if (r < 0) {
        eb_forget(b, eb);
        return -EIO;
    }
    *out = eb;
    return 0;
}

void eb_mark_dirty(Btrfs *b, ExtentBuffer *eb)
{
    if (eb->dirty)
        return;
    eb->dirty = 1;
    if (push(&b->dirty_ebs, &b->ndirty, &b->cap_dirty, eb) < 0)
        b->error = -ENOMEM;
}

/* btrfs_write_and_wait_transaction: every dirty block, checksummed. */
int write_dirty_tree_blocks(Btrfs *b)
{
    for (size_t i = 0; i < b->ndirty; i++) {
        ExtentBuffer *eb = b->dirty_ebs[i];
        if (!eb->dirty)
            continue;
        eb->dirty = 0;
        if (eb->stale)
            continue; /* freed before it was ever written */
        eb_set_flags(eb, eb_flags(eb) | BTRFS_HEADER_FLAG_WRITTEN);
        csum_tree_block(eb->data, b->nodesize);
        TRY(disk_write_bytes(b->disk, eb->start, eb->data, b->nodesize));
        b->stats.tree_blocks_written++;
    }
    b->ndirty = 0;
    return 0;
}

void eb_free_stale(Btrfs *b)
{
    /* Called after write_dirty_tree_blocks emptied the dirty list, when no
     * path points at any buffer. */
    for (size_t i = 0; i < b->nstale; i++) {
        free(b->stale_ebs[i]->data);
        free(b->stale_ebs[i]);
    }
    b->nstale = 0;
}

void eb_cache_destroy(Btrfs *b)
{
    for (size_t i = 0; i < b->ebhash_n; i++)
        for (ExtentBuffer *e = b->ebhash[i], *next; e; e = next) {
            next = e->hnext;
            free(e->data);
            free(e);
        }
    for (size_t i = 0; i < b->nstale; i++) {
        free(b->stale_ebs[i]->data);
        free(b->stale_ebs[i]);
    }
    free(b->ebhash);
    free(b->dirty_ebs);
    free(b->stale_ebs);
    b->ebhash = NULL;
    b->dirty_ebs = b->stale_ebs = NULL;
    b->ebhash_n = b->eb_count = b->ndirty = b->nstale = 0;
}
