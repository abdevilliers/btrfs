#include "recovery.h"
#include <string.h>

typedef struct {
    uint32_t  next_id, pad;
    RootEntry entry[MAX_CKPT];
} TableBlock;

_Static_assert(sizeof(TableBlock) <= BLOCK_SIZE, "root table must fit a block");
_Static_assert(NBLOCKS * sizeof(uint32_t) <= BLOCK_SIZE, "refcounts must fit a block");

uint32_t crc32(const void *data, uint32_t len)
{
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint32_t sb_csum(Superblock sb) { sb.csum = 0; return crc32(&sb, sizeof sb); }

Volume *rec_format(Disk *d)
{
    uint8_t zero[BLOCK_SIZE] = {0};
    disk_write(d, SB_PRIMARY, zero);
    disk_write(d, SB_MIRROR, zero);
    Volume *v = vol_create(d);
    v->sb_slot = 1;          /* so the first commit uses slot 0 */
    rec_commit(v);
    return v;
}

int rec_commit(Volume *v)
{
    uint8_t buf[BLOCK_SIZE];
    uint32_t slot = v->sb_slot ^ 1u;                 /* the spare slot */

    /* 2. root table + refcounts into the spare slot */
    memset(buf, 0, sizeof buf);
    TableBlock *tb = (TableBlock *)buf;
    tb->next_id = v->next_id;
    memcpy(tb->entry, v->table, sizeof v->table);
    disk_write(v->disk, SLOT_BASE + 2 * slot, buf);

    memset(buf, 0, sizeof buf);
    memcpy(buf, alloc_table(v->alloc), NBLOCKS * sizeof(uint32_t));
    disk_write(v->disk, SLOT_BASE + 2 * slot + 1, buf);

    disk_flush(v->disk);                             /* 3. flush */

    /* 4. superblock, then mirror */
    Superblock sb = { SB_MAGIC, 0, v->generation + 1, v->live.root, slot,
                      NBLOCKS, BLOCK_SIZE };
    sb.csum = sb_csum(sb);
    memset(buf, 0, sizeof buf);
    memcpy(buf, &sb, sizeof sb);
    disk_write(v->disk, SB_PRIMARY, buf);
    disk_flush(v->disk);
    disk_write(v->disk, SB_MIRROR, buf);
    disk_flush(v->disk);

    alloc_commit(v->alloc);                          /* 5. */
    v->generation = sb.generation;
    v->sb_slot = slot;
    return 0;
}

static int sb_load(Disk *d, uint32_t blk, Superblock *sb)
{
    uint8_t buf[BLOCK_SIZE];
    disk_read(d, blk, buf);
    memcpy(sb, buf, sizeof *sb);
    return sb->magic == SB_MAGIC && sb->csum == sb_csum(*sb) && sb->slot < 2;
}

int rec_mount(Disk *d, Volume **out)
{
    Superblock a, b, sb;
    int va = sb_load(d, SB_PRIMARY, &a);            /* 1. read both */
    int vb = sb_load(d, SB_MIRROR, &b);
    int used;                                        /* 2. newest valid */
    if (va && (!vb || a.generation >= b.generation)) { sb = a; used = 0; }
    else if (vb)                                      { sb = b; used = 1; }
    else return -1;

    Volume *v = vol_create(d);                       /* 3. load slot */
    uint8_t buf[BLOCK_SIZE];
    disk_read(d, SLOT_BASE + 2 * sb.slot, buf);
    TableBlock *tb = (TableBlock *)buf;
    v->next_id = tb->next_id;
    memcpy(v->table, tb->entry, sizeof v->table);

    disk_read(d, SLOT_BASE + 2 * sb.slot + 1, buf);
    alloc_load(v->alloc, (const uint32_t *)buf);

    v->live.root = sb.live_root;                     /* 4. resume */
    v->generation = sb.generation;
    v->sb_slot = sb.slot;
    *out = v;
    return used;
}
