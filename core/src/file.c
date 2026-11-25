/*
 * file.c - file data (fs/btrfs/file.c, inode.c, file-item.c, reflink.c).
 *
 * A file's data is described by EXTENT_DATA items (ino, EXTENT_DATA, file
 * offset), each pointing at part of a data extent on the disk:
 *
 *     file  [ key.offset ............ key.offset + num_bytes )
 *     disk  [ disk_bytenr + offset .. ) inside the extent
 *           [ disk_bytenr, disk_bytenr + disk_num_bytes )
 *
 * Small files (up to 2048 bytes) are stored INLINE, in the item itself.
 * Data is never overwritten: a write puts the new sectors in a newly
 * allocated extent, then __btrfs_drop_extents cuts the old items out of
 * the written range (splitting an item that sticks out on both sides),
 * then the new item is inserted. Each data sector's crc32c goes in the
 * csum tree, (EXTENT_CSUM, EXTENT_CSUM, bytenr), and is checked on read.
 * Holes are simply missing items (the NO_HOLES feature).
 *
 * Simplification: a write goes to the disk at once (btrfs collects dirty
 * pages and allocates when it writes them back, "delalloc").
 */
#include <errno.h>
#include <stdlib.h>

#include "ctree.h"

#define MAX_INLINE 2048u
#define CHUNK (1024u * 1024u) /* writes are done 1 MiB at a time */

static uint64_t round_down(uint64_t v) { return v / SECTORSIZE * SECTORSIZE; }
static uint64_t round_up(uint64_t v) { return (v + SECTORSIZE - 1) / SECTORSIZE * SECTORSIZE; }

static int is_dir_mode(uint32_t mode) { return (mode & 0170000u) == 0040000u; }

/* ---------------------------------------------------------- csum tree */

static uint32_t max_csums(const Btrfs *b)
{
    return (leaf_data_size(b) - ITEM_SIZE) / 4 - 1;
}

int csum_insert(Trans *t, uint64_t bytenr, const uint8_t *data, uint64_t len)
{
    Btrfs *b = t->b;
    uint32_t per = max_csums(b);
    uint32_t *sums = malloc((size_t)per * 4);
    if (!sums)
        return -ENOMEM;
    int r = 0;
    for (uint64_t pos = 0; pos < len && r == 0;) {
        uint64_t n = (len - pos) / SECTORSIZE;
        if (n > per)
            n = per;
        for (uint64_t i = 0; i < n; i++)
            sums[i] = btrfs_csum(data + pos + i * SECTORSIZE, SECTORSIZE);
        BtrfsKey k = mkkey(BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, bytenr + pos);
        r = btrfs_insert_item(t, b->csum_root, &k, sums, (uint32_t)(n * 4));
        pos += n * SECTORSIZE;
    }
    free(sums);
    return r;
}

/* btrfs_del_csums: remove the checksums of [bytenr, bytenr + len). */
int csum_delete(Trans *t, uint64_t bytenr, uint64_t len)
{
    Btrfs *b = t->b;
    uint64_t end = bytenr + len;
    for (;;) {
        Path p;
        BtrfsKey k = mkkey(BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, end - 1), f;
        if (btrfs_search_back(b, b->csum_root, &k, &p) != 0)
            return 0;
        item_key(p.nodes[0], p.slots[0], &f);
        if (f.objectid != BTRFS_EXTENT_CSUM_OBJECTID || f.type != BTRFS_EXTENT_CSUM_KEY)
            return 0;
        uint32_t size = item_size(p.nodes[0], p.slots[0]);
        uint64_t s = f.offset, e = s + (uint64_t)(size / 4) * SECTORSIZE;
        if (e <= bytenr)
            return 0;
        uint8_t *sums = malloc(size);
        if (!sums)
            return -ENOMEM;
        memcpy(sums, item_ptr(p.nodes[0], p.slots[0]), size);
        int r = btrfs_delete_key(t, b->csum_root, &f);
        if (r == 0 && s < bytenr) {
            BtrfsKey l = f;
            r = btrfs_insert_item(t, b->csum_root, &l, sums, (uint32_t)((bytenr - s) / SECTORSIZE * 4));
        }
        if (r == 0 && e > end) {
            BtrfsKey rk = mkkey(BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, end);
            r = btrfs_insert_item(t, b->csum_root, &rk, sums + (end - s) / SECTORSIZE * 4,
                                  (uint32_t)((e - end) / SECTORSIZE * 4));
        }
        free(sums);
        if (r < 0)
            return r;
    }
}

int csum_lookup(Btrfs *b, uint64_t bytenr, uint32_t *csum)
{
    Path p;
    BtrfsKey k = mkkey(BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, bytenr), f;
    if (btrfs_search_back(b, b->csum_root, &k, &p) != 0)
        return -ENOENT;
    item_key(p.nodes[0], p.slots[0], &f);
    if (f.objectid != BTRFS_EXTENT_CSUM_OBJECTID || f.type != BTRFS_EXTENT_CSUM_KEY)
        return -ENOENT;
    uint64_t idx = (bytenr - f.offset) / SECTORSIZE;
    if (idx >= item_size(p.nodes[0], p.slots[0]) / 4)
        return -ENOENT;
    *csum = get32(item_ptr(p.nodes[0], p.slots[0]) + idx * 4);
    return 0;
}

/* ------------------------------------------------- file extent items */

typedef struct {
    BtrfsKey key;
    BtrfsFileExtentItem fi;
    uint32_t inline_len; /* data bytes of an inline extent */
} Ext;

static void read_ext(const ExtentBuffer *eb, int slot, Ext *e)
{
    uint32_t size = item_size(eb, slot);
    item_key(eb, slot, &e->key);
    memset(&e->fi, 0, sizeof e->fi);
    memcpy(&e->fi, item_ptr(eb, slot), size < sizeof e->fi ? size : sizeof e->fi);
    e->inline_len = e->fi.type == BTRFS_FILE_EXTENT_INLINE ? size - BTRFS_FILE_EXTENT_INLINE_DATA_START : 0;
}

static uint64_t ext_end(const Ext *e)
{
    return e->key.offset + (e->fi.type == BTRFS_FILE_EXTENT_INLINE ? e->fi.ram_bytes : e->fi.num_bytes);
}

/* The first extent item of `in` that ends after `start` and begins before
 * `end`. 0 = found, 1 = none. */
static int find_overlap(Btrfs *b, Inode in, uint64_t start, uint64_t end, Ext *e)
{
    Path p;
    BtrfsKey k = mkkey(in.ino, BTRFS_EXTENT_DATA_KEY, start);
    if (btrfs_search_back(b, in.root, &k, &p) == 0) {
        read_ext(p.nodes[0], p.slots[0], e);
        if (e->key.objectid == in.ino && e->key.type == BTRFS_EXTENT_DATA_KEY && ext_end(e) > start)
            return 0;
    }
    int r = btrfs_search_forward(b, in.root, &k, &p);
    if (r != 0)
        return r < 0 ? r : 1;
    read_ext(p.nodes[0], p.slots[0], e);
    if (e->key.objectid == in.ino && e->key.type == BTRFS_EXTENT_DATA_KEY && e->key.offset < end)
        return 0;
    return 1;
}

static int put_fi(Trans *t, Root *root, const BtrfsKey *k, const BtrfsFileExtentItem *fi)
{
    return btrfs_update_item(t, root, k, fi, sizeof *fi);
}

/* __btrfs_drop_extents: no extent item of `in` covers [start, end) any more. */
int drop_extents(Trans *t, Inode in, uint64_t start, uint64_t end, uint64_t *dropped)
{
    Btrfs *b = t->b;
    uint64_t rid = in.root->objectid;
    Ext e;
    *dropped = 0;

    for (;;) {
        int r = find_overlap(b, in, start, end, &e);
        if (r)
            return r < 0 ? r : 0;
        uint64_t is = e.key.offset, ie = ext_end(&e);
        BtrfsFileExtentItem fi = e.fi;

        if (fi.type == BTRFS_FILE_EXTENT_INLINE) {
            TRY(btrfs_delete_key(t, in.root, &e.key));
            *dropped += fi.ram_bytes;
            continue;
        }
        if (is < start && ie > end) {
            /* Sticks out on both sides: keep the head, add an item for the tail. */
            fi.num_bytes = start - is;
            TRY(put_fi(t, in.root, &e.key, &fi));
            BtrfsFileExtentItem tail = e.fi;
            tail.offset += end - is;
            tail.num_bytes = ie - end;
            BtrfsKey tk = mkkey(in.ino, BTRFS_EXTENT_DATA_KEY, end);
            TRY(btrfs_insert_item(t, in.root, &tk, &tail, sizeof tail));
            if (tail.disk_bytenr)
                TRY(add_data_ref(t, tail.disk_bytenr, tail.disk_num_bytes, rid, in.ino,
                                 end - tail.offset, 1, 0));
            *dropped += end - start;
        } else if (is < start) {
            fi.num_bytes = start - is; /* keep the head */
            TRY(put_fi(t, in.root, &e.key, &fi));
            *dropped += ie - start;
        } else if (ie > end) {
            /* Keep the tail: the item now starts at `end`. */
            Path p;
            BtrfsKey nk = mkkey(in.ino, BTRFS_EXTENT_DATA_KEY, end);
            fi.offset += end - is;
            fi.num_bytes = ie - end;
            r = btrfs_search_slot(t, in.root, &e.key, &p, 0, 1);
            if (r != 0)
                return r < 0 ? r : -EIO;
            memcpy(item_ptr(p.nodes[0], p.slots[0]), &fi, sizeof fi);
            btrfs_set_item_key_safe(t, &p, &nk);
            *dropped += end - is;
        } else {
            TRY(btrfs_delete_key(t, in.root, &e.key));
            if (fi.disk_bytenr)
                TRY(add_data_ref(t, fi.disk_bytenr, fi.disk_num_bytes, rid, in.ino, is - fi.offset, -1, 0));
            *dropped += fi.num_bytes;
        }
    }
}

/* btrfs_truncate_inode_items: drop everything past new_size. A regular
 * extent keeps its last partial sector. */
int truncate_inode_items(Trans *t, Inode in, BtrfsInodeItem *ii, uint64_t new_size)
{
    Btrfs *b = t->b;
    uint64_t aligned = round_up(new_size);
    for (;;) {
        Path p;
        Ext e;
        BtrfsKey k = mkkey(in.ino, BTRFS_EXTENT_DATA_KEY, (uint64_t)-1);
        if (btrfs_search_back(b, in.root, &k, &p) != 0)
            return 0;
        read_ext(p.nodes[0], p.slots[0], &e);
        if (e.key.objectid != in.ino || e.key.type != BTRFS_EXTENT_DATA_KEY)
            return 0;
        uint64_t is = e.key.offset, ie = ext_end(&e);
        if (e.fi.type == BTRFS_FILE_EXTENT_INLINE) {
            if (ie <= new_size)
                return 0;
            if (new_size == 0) {
                TRY(btrfs_delete_key(t, in.root, &e.key));
            } else {
                int r = btrfs_search_slot(t, in.root, &e.key, &p, 0, 1);
                if (r != 0)
                    return r < 0 ? r : -EIO;
                btrfs_truncate_item(t, &p, BTRFS_FILE_EXTENT_INLINE_DATA_START + (uint32_t)new_size, 1);
                put64(item_ptr(p.nodes[0], p.slots[0]) + 8, new_size); /* ram_bytes */
            }
            ii->nbytes -= ie - new_size;
            return 0;
        }
        if (ie <= aligned)
            return 0;
        if (is >= aligned) {
            TRY(btrfs_delete_key(t, in.root, &e.key));
            if (e.fi.disk_bytenr)
                TRY(add_data_ref(t, e.fi.disk_bytenr, e.fi.disk_num_bytes, in.root->objectid, in.ino,
                                 is - e.fi.offset, -1, 0));
            ii->nbytes -= e.fi.num_bytes;
            continue;
        }
        e.fi.num_bytes = aligned - is;
        TRY(put_fi(t, in.root, &e.key, &e.fi));
        ii->nbytes -= ie - aligned;
        return 0;
    }
}

/* ------------------------------------------------------------- reading */

static int read_sector(Btrfs *b, uint64_t phys, uint8_t *buf)
{
    uint32_t want;
    TRY(disk_read_bytes(b->disk, phys, buf, SECTORSIZE));
    if (csum_lookup(b, phys, &want) < 0)
        return -EIO; /* data with no checksum */
    return btrfs_csum(buf, SECTORSIZE) == want ? 0 : -EIO;
}

int read_range(Btrfs *b, Inode in, uint64_t off, uint8_t *buf, uint64_t len)
{
    uint8_t *sector = malloc(SECTORSIZE);
    Ext e;
    uint64_t pos = off, end = off + len;
    int r = 0;

    if (!sector)
        return -ENOMEM;
    memset(buf, 0, len);
    while (pos < end && r == 0) {
        int f = find_overlap(b, in, pos, end, &e);
        if (f) {
            r = f < 0 ? f : 0;
            break;
        }
        uint64_t is = e.key.offset, ie = ext_end(&e);
        uint64_t a = is > pos ? is : pos, z = ie < end ? ie : end;
        if (e.fi.type == BTRFS_FILE_EXTENT_INLINE) {
            Path p;
            if (btrfs_search_forward(b, in.root, &e.key, &p) == 0) {
                const uint8_t *d = item_ptr(p.nodes[0], p.slots[0]) + BTRFS_FILE_EXTENT_INLINE_DATA_START;
                uint64_t avail = e.inline_len < e.fi.ram_bytes ? e.inline_len : e.fi.ram_bytes;
                if (a - is < avail)
                    memcpy(buf + (a - off), d + (a - is), (z - is < avail ? z : is + avail) - a);
            }
        } else if (e.fi.disk_bytenr) {
            uint64_t phys = e.fi.disk_bytenr + e.fi.offset + (a - is);
            for (uint64_t at = a; at < z && r == 0;) {
                uint64_t ps = round_down(phys + (at - a));
                uint64_t in_sec = phys + (at - a) - ps;
                uint64_t n = SECTORSIZE - in_sec < z - at ? SECTORSIZE - in_sec : z - at;
                r = read_sector(b, ps, sector);
                if (r == 0)
                    memcpy(buf + (at - off), sector + in_sec, n);
                at += n;
            }
        }
        pos = z;
    }
    free(sector);
    return r;
}

static int get_file(Fs *fs, const char *path, Inode *in, BtrfsInodeItem *ii)
{
    TRY(resolve_path(fs, path, in));
    TRY(inode_get(fs->b, *in, ii));
    return is_dir_mode(ii->mode) ? -EISDIR : 0;
}

int64_t fs_read(Fs *fs, const char *path, uint64_t off, void *buf, uint64_t len)
{
    Inode in;
    BtrfsInodeItem ii;
    TRY(get_file(fs, path, &in, &ii));
    if (off >= ii.size)
        return 0;
    if (len > ii.size - off)
        len = ii.size - off;
    TRY(read_range(fs->b, in, off, buf, len));
    return (int64_t)len;
}

/* ------------------------------------------------------------- writing */

static void touch(BtrfsInodeItem *ii, uint64_t transid)
{
    ii->transid = transid;
    ii->sequence++;
    ii->mtime.sec = ii->ctime.sec = now_sec();
}

/* What extents the file has: an inline one (at 0), regular ones. */
static void file_shape(Btrfs *b, Inode in, int *has_inline, int *has_regular)
{
    Ext e;
    *has_inline = *has_regular = 0;
    if (find_overlap(b, in, 0, (uint64_t)-1, &e) == 0) {
        if (e.fi.type == BTRFS_FILE_EXTENT_INLINE)
            *has_inline = 1;
        else
            *has_regular = 1;
    }
}

/* The whole file becomes one inline extent of `size` bytes. */
static int write_inline(Btrfs *b, Inode in, BtrfsInodeItem *ii, const uint8_t *data, uint64_t size)
{
    Trans t;
    uint64_t dropped;
    uint8_t item[BTRFS_FILE_EXTENT_INLINE_DATA_START + MAX_INLINE];
    BtrfsFileExtentItem fi = { 0 };

    TRY(trans_start(b, &t, 4));
    int r = drop_extents(&t, in, 0, SECTORSIZE, &dropped);
    if (!r && size) {
        fi.generation = b->transid;
        fi.ram_bytes = size;
        fi.type = BTRFS_FILE_EXTENT_INLINE;
        memcpy(item, &fi, BTRFS_FILE_EXTENT_INLINE_DATA_START);
        memcpy(item + BTRFS_FILE_EXTENT_INLINE_DATA_START, data, size);
        BtrfsKey k = mkkey(in.ino, BTRFS_EXTENT_DATA_KEY, 0);
        r = btrfs_insert_item(&t, in.root, &k, item, (uint32_t)(BTRFS_FILE_EXTENT_INLINE_DATA_START + size));
    }
    if (!r) {
        ii->nbytes = ii->nbytes - dropped + size;
        ii->size = size;
        touch(ii, b->transid);
        r = inode_put(&t, in, ii);
    }
    trans_end(&t);
    return r < 0 ? trans_abort(&t, r) : 0;
}

/* Write whole sectors [start, start + len) to new extents (copy-on-write). */
static int write_regular(Btrfs *b, Inode in, BtrfsInodeItem *ii, uint64_t start, const uint8_t *data,
                         uint64_t len, uint64_t new_size)
{
    Range ext[CHUNK / BLOCK_SIZE + 1];
    size_t n = 0;
    uint64_t got = 0, dropped;
    Trans t;
    int r;

    TRY(reserve_data_space(b, len));
    while (got < len) {
        uint64_t s, l;
        r = n < sizeof ext / sizeof *ext ? alloc_data_extent(b, len - got, &s, &l) : -ENOSPC;
        if (r < 0) {
            for (size_t i = 0; i < n; i++)
                unalloc_data_extent(b, ext[i].start, ext[i].len);
            release_data_space(b, len);
            return r;
        }
        ext[n].start = s;
        ext[n].len = l;
        n++;
        got += l;
    }
    r = trans_start(b, &t, 4 + 2 * (unsigned)n);
    if (r < 0) {
        for (size_t i = 0; i < n; i++)
            unalloc_data_extent(b, ext[i].start, ext[i].len);
        release_data_space(b, len);
        return r;
    }
    uint64_t pos = 0;
    for (size_t i = 0; i < n && r == 0; i++) {
        r = disk_write_bytes(b->disk, ext[i].start, data + pos, ext[i].len);
        b->stats.data_sectors_written += ext[i].len / SECTORSIZE;
        if (!r)
            r = csum_insert(&t, ext[i].start, data + pos, ext[i].len);
        pos += ext[i].len;
    }
    if (!r)
        r = drop_extents(&t, in, start, start + len, &dropped);
    pos = 0;
    for (size_t i = 0; i < n && r == 0; i++) {
        BtrfsFileExtentItem fi = { 0 };
        fi.generation = b->transid;
        fi.ram_bytes = ext[i].len;
        fi.type = BTRFS_FILE_EXTENT_REG;
        fi.disk_bytenr = ext[i].start;
        fi.disk_num_bytes = ext[i].len;
        fi.num_bytes = ext[i].len;
        BtrfsKey k = mkkey(in.ino, BTRFS_EXTENT_DATA_KEY, start + pos);
        r = btrfs_insert_item(&t, in.root, &k, &fi, sizeof fi);
        if (!r)
            r = add_data_ref(&t, ext[i].start, ext[i].len, in.root->objectid, in.ino, start + pos, 1, 1);
        pos += ext[i].len;
    }
    if (!r) {
        ii->nbytes = ii->nbytes + len - dropped;
        ii->size = new_size;
        touch(ii, b->transid);
        r = inode_put(&t, in, ii);
    }
    trans_end(&t);
    return r < 0 ? trans_abort(&t, r) : 0;
}

/* An inline extent becomes a regular one (the file grows past it). */
static int convert_inline(Btrfs *b, Inode in, BtrfsInodeItem *ii)
{
    uint8_t *sec = calloc(1, SECTORSIZE);
    if (!sec)
        return -ENOMEM;
    int r = read_range(b, in, 0, sec, ii->size < SECTORSIZE ? ii->size : SECTORSIZE);
    if (!r)
        r = write_regular(b, in, ii, 0, sec, SECTORSIZE, ii->size);
    free(sec);
    return r;
}

static int write_chunk(Btrfs *b, Inode in, uint64_t off, const uint8_t *data, uint64_t len)
{
    BtrfsInodeItem ii;
    int has_inline, has_regular;
    TRY(inode_get(b, in, &ii));
    uint64_t end = off + len, new_size = ii.size > end ? ii.size : end;
    file_shape(b, in, &has_inline, &has_regular);

    if (!has_regular && new_size <= MAX_INLINE) {
        uint8_t content[MAX_INLINE];
        memset(content, 0, sizeof content);
        TRY(read_range(b, in, 0, content, ii.size < new_size ? ii.size : new_size));
        memcpy(content + off, data, len);
        return write_inline(b, in, &ii, content, new_size);
    }
    uint64_t s = round_down(off), e = round_up(end);
    if (has_inline && s > 0) {
        TRY(convert_inline(b, in, &ii));
        TRY(inode_get(b, in, &ii));
    }
    uint8_t *buf = calloc(1, e - s);
    if (!buf)
        return -ENOMEM;
    int r = 0;
    /* Read-modify-write of the partial sectors at either end. */
    if (off > s && s < ii.size)
        r = read_range(b, in, s, buf, ii.size - s < SECTORSIZE ? ii.size - s : SECTORSIZE);
    uint64_t ls = e - SECTORSIZE;
    if (!r && end < e && ls < ii.size && !(off > s && ls == s)) {
        r = read_range(b, in, ls, buf + (ls - s), ii.size - ls < SECTORSIZE ? ii.size - ls : SECTORSIZE);
    }
    if (!r) {
        memcpy(buf + (off - s), data, len);
        r = write_regular(b, in, &ii, s, buf, e - s, new_size);
    }
    free(buf);
    return r;
}

int64_t fs_write(Fs *fs, const char *path, uint64_t offset, const void *buf, uint64_t len)
{
    Inode in;
    BtrfsInodeItem ii;
    TRY(get_file(fs, path, &in, &ii));
    TRY(fs_root_writable(fs, in.root));
    if (offset + len < offset || offset + len > (1ULL << 62))
        return -EFBIG;
    for (uint64_t done = 0; done < len;) {
        uint64_t n = len - done < CHUNK ? len - done : CHUNK;
        int r = write_chunk(fs->b, in, offset + done, (const uint8_t *)buf + done, n);
        if (r < 0)
            return done ? (int64_t)done : r;
        done += n;
    }
    return (int64_t)len;
}

int fs_truncate(Fs *fs, const char *path, uint64_t size)
{
    Btrfs *b = fs->b;
    Inode in;
    BtrfsInodeItem ii;
    int has_inline, has_regular;
    Trans t;

    TRY(get_file(fs, path, &in, &ii));
    TRY(fs_root_writable(fs, in.root));
    if (size == ii.size)
        return 0;
    file_shape(b, in, &has_inline, &has_regular);
    if (!has_regular && size <= MAX_INLINE) {
        uint8_t content[MAX_INLINE];
        memset(content, 0, sizeof content);
        TRY(read_range(b, in, 0, content, ii.size < size ? ii.size : size));
        return write_inline(b, in, &ii, content, size);
    }
    if (has_inline) {
        TRY(convert_inline(b, in, &ii));
        TRY(inode_get(b, in, &ii));
    }
    if (size < ii.size && size % SECTORSIZE) {
        /* btrfs_truncate_block: zero the rest of the new last sector. */
        uint64_t ls = round_down(size);
        uint8_t *sec = calloc(1, SECTORSIZE);
        if (!sec)
            return -ENOMEM;
        int r = read_range(b, in, ls, sec, ii.size - ls < SECTORSIZE ? ii.size - ls : SECTORSIZE);
        memset(sec + size % SECTORSIZE, 0, SECTORSIZE - size % SECTORSIZE);
        if (!r)
            r = write_regular(b, in, &ii, ls, sec, SECTORSIZE, ii.size);
        free(sec);
        TRY(r);
        TRY(inode_get(b, in, &ii));
    }
    TRY(trans_start_fallback_global(b, &t, 4));
    int r = size < ii.size ? truncate_inode_items(&t, in, &ii, size) : 0;
    if (!r) {
        ii.size = size;
        touch(&ii, b->transid);
        r = inode_put(&t, in, &ii);
    }
    trans_end(&t);
    return r < 0 ? trans_abort(&t, r) : 0;
}

/* ------------------------------------------------------------- reflink */

static int collect_exts(Btrfs *b, Inode in, uint64_t start, uint64_t end, Ext **out, size_t *n)
{
    size_t cap = 0;
    *out = NULL;
    *n = 0;
    for (uint64_t pos = start; pos < end;) {
        Ext e;
        int r = find_overlap(b, in, pos, end, &e);
        if (r)
            return r < 0 ? r : 0;
        if (*n == cap) {
            cap = cap ? cap * 2 : 16;
            Ext *ne = realloc(*out, cap * sizeof *ne);
            if (!ne)
                return -ENOMEM;
            *out = ne;
        }
        (*out)[(*n)++] = e;
        pos = ext_end(&e);
    }
    return 0;
}

int fs_clone_range(Fs *fs, const char *src, uint64_t src_off, const char *dst, uint64_t dst_off, uint64_t len)
{
    Btrfs *b = fs->b;
    Inode si, di;
    BtrfsInodeItem sii, dii;
    int s_inline, s_reg, d_inline, d_reg;
    Trans t;

    TRY(get_file(fs, src, &si, &sii));
    TRY(get_file(fs, dst, &di, &dii));
    TRY(fs_root_writable(fs, di.root));
    if (len == 0) {
        if (src_off > sii.size)
            return -EINVAL;
        len = sii.size - src_off;
    }
    if (len == 0)
        return 0;
    if (src_off % SECTORSIZE || dst_off % SECTORSIZE || src_off + len > sii.size)
        return -EINVAL;
    if (len % SECTORSIZE && !(src_off + len == sii.size && dst_off + len >= dii.size))
        return -EINVAL; /* an unaligned end must be the end of both files */
    if (si.root == di.root && si.ino == di.ino && src_off < dst_off + len && dst_off < src_off + len)
        return -EINVAL;

    file_shape(b, si, &s_inline, &s_reg);
    file_shape(b, di, &d_inline, &d_reg);
    if (s_inline || d_inline) {
        /* Inline data lives in the tree: copy it (clone_copy_inline_extent). */
        uint8_t *buf = malloc(len);
        if (!buf)
            return -ENOMEM;
        int r = read_range(b, si, src_off, buf, len);
        int64_t w = r < 0 ? r : fs_write(fs, dst, dst_off, buf, len);
        free(buf);
        return w < 0 ? (int)w : 0;
    }

    uint64_t alen = round_up(len), dropped, added = 0;
    Ext *exts;
    size_t n;
    TRY(collect_exts(b, si, src_off, src_off + alen, &exts, &n));
    int r = trans_start(b, &t, 3 + 2 * (unsigned)n);
    if (r < 0) {
        free(exts);
        return r;
    }
    r = drop_extents(&t, di, dst_off, dst_off + alen, &dropped);
    for (size_t i = 0; i < n && r == 0; i++) {
        uint64_t is = exts[i].key.offset, ie = ext_end(&exts[i]);
        uint64_t a = is > src_off ? is : src_off, z = ie < src_off + alen ? ie : src_off + alen;
        if (a >= z || !exts[i].fi.disk_bytenr)
            continue;
        BtrfsFileExtentItem fi = exts[i].fi;
        fi.offset += a - is;
        fi.num_bytes = z - a;
        fi.generation = b->transid;
        BtrfsKey k = mkkey(di.ino, BTRFS_EXTENT_DATA_KEY, dst_off + (a - src_off));
        r = btrfs_insert_item(&t, di.root, &k, &fi, sizeof fi);
        if (!r)
            r = add_data_ref(&t, fi.disk_bytenr, fi.disk_num_bytes, di.root->objectid, di.ino,
                             k.offset - fi.offset, 1, 0);
        added += z - a;
    }
    free(exts);
    if (!r)
        r = inode_get(b, di, &dii);
    if (!r) {
        dii.nbytes = dii.nbytes + added - dropped;
        if (dst_off + len > dii.size)
            dii.size = dst_off + len;
        touch(&dii, b->transid);
        r = inode_put(&t, di, &dii);
    }
    trans_end(&t);
    return r < 0 ? trans_abort(&t, r) : 0;
}

/* -------------------------------------------------------------- fiemap */

int fs_fiemap(Fs *fs, const char *path, fs_extent_fn fn, void *arg)
{
    Btrfs *b = fs->b;
    Inode in;
    BtrfsInodeItem ii;
    Path p;
    FsExtent *list = NULL;
    size_t n = 0, cap = 0;

    TRY(get_file(fs, path, &in, &ii));
    BtrfsKey k = mkkey(in.ino, BTRFS_EXTENT_DATA_KEY, 0);
    int r = btrfs_search_forward(b, in.root, &k, &p);
    while (r == 0) {
        Ext e;
        read_ext(p.nodes[0], p.slots[0], &e);
        if (e.key.objectid != in.ino || e.key.type != BTRFS_EXTENT_DATA_KEY)
            break;
        FsExtent x = { e.key.offset, 0, 0, 0, 0 };
        if (e.fi.type == BTRFS_FILE_EXTENT_INLINE) {
            x.length = e.fi.ram_bytes;
            x.flags = FIEMAP_EXTENT_INLINE;
        } else if (e.fi.disk_bytenr) {
            uint64_t refs, flags;
            x.physical = e.fi.disk_bytenr + e.fi.offset;
            x.extent_bytenr = e.fi.disk_bytenr;
            x.length = e.fi.num_bytes;
            /* Shared: the extent has other references, or a tree block on
             * the way to this item is shared (btrfs_is_data_extent_shared). */
            if (lookup_extent_info(b, e.fi.disk_bytenr, 0, 1, &refs, &flags) == 0 && refs > 1)
                x.flags |= FIEMAP_EXTENT_SHARED;
            for (int l = 0; l < BTRFS_MAX_LEVEL && p.nodes[l] && !(x.flags & FIEMAP_EXTENT_SHARED); l++)
                if (p.nodes[l] != in.root->node &&
                    lookup_extent_info(b, p.nodes[l]->start, l, 0, &refs, &flags) == 0 && refs > 1)
                    x.flags |= FIEMAP_EXTENT_SHARED;
        }
        if (x.length) {
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                FsExtent *nl = realloc(list, cap * sizeof *nl);
                if (!nl) {
                    free(list);
                    return -ENOMEM;
                }
                list = nl;
            }
            list[n++] = x;
        }
        r = btrfs_next_item(b, &p);
    }
    for (size_t i = 0; i < n; i++)
        fn(arg, &list[i]);
    free(list);
    return r < 0 ? r : 0;
}
