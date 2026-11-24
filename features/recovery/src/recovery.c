/*
 * recovery.c - commit and mount, using a two-slot superblock.
 *
 * Disk layout (blocks 1 .. VOL_META_BLOCKS are set aside by core):
 *
 *     | slot 0: SLOT_BLOCKS blocks | slot 1: SLOT_BLOCKS blocks | pages and trees ...
 *
 * Commit number g is written to slot g % 2, so two commits in a row never
 * touch the same slot.
 */
#include <errno.h>
#include <string.h>

#include "recovery.h"

#define SLOT_BLOCKS (VOL_META_BLOCKS / 2)
#define SLOT_BYTES (SLOT_BLOCKS * BLOCK_SIZE)
#define MAGIC 0x53465442u /* "BTFS" */

typedef struct {
    uint32_t magic;      /* marks a superblock                         */
    uint32_t checksum;   /* of the whole struct, with this field as 0  */
    uint32_t generation; /* commit number: the bigger one is newer     */
    uint32_t state_len;
    uint8_t state[SLOT_BYTES - 16]; /* the volume's state, from vol_save */
} Superblock;

_Static_assert(sizeof(Superblock) == SLOT_BYTES, "a superblock fills one slot exactly");
_Static_assert(sizeof(((Superblock *)0)->state) >= VOL_STATE_MAX, "slot too small for the state");

/* FNV-1a: a short checksum that changes if any byte changes. */
static uint32_t checksum(const Superblock *sb)
{
    Superblock copy = *sb;
    const uint8_t *p = (const uint8_t *)&copy;
    uint32_t h = 2166136261u;

    copy.checksum = 0;
    for (size_t i = 0; i < sizeof copy; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static uint32_t slot_start(uint32_t slot)
{
    return 1 + slot * SLOT_BLOCKS;
}

/* Read one slot. Returns 1 if it holds a whole, undamaged superblock. */
static int read_slot(Disk *disk, uint32_t slot, Superblock *sb)
{
    uint8_t *p = (uint8_t *)sb;
    for (uint32_t i = 0; i < SLOT_BLOCKS; i++)
        disk_read(disk, slot_start(slot) + i, p + i * BLOCK_SIZE);
    return sb->magic == MAGIC && sb->checksum == checksum(sb);
}

/* Read both slots and keep the newest good one. Returns 0 if neither is. */
static int read_newest(Disk *disk, Superblock *sb)
{
    Superblock other;
    int ok0 = read_slot(disk, 0, sb);
    int ok1 = read_slot(disk, 1, &other);

    if (ok1 && (!ok0 || other.generation > sb->generation))
        *sb = other;
    return ok0 || ok1;
}

int rec_commit(Volume *v)
{
    Disk *disk = vol_disk(v);
    Superblock sb;
    uint32_t last = read_newest(disk, &sb) ? sb.generation : 0;

    /* 1. Describe the state to commit. */
    memset(&sb, 0, sizeof sb);
    sb.magic = MAGIC;
    sb.generation = last + 1;
    int len = vol_save(v, sb.state, sizeof sb.state);
    if (len < 0)
        return len;
    sb.state_len = (uint32_t)len;
    sb.checksum = checksum(&sb);

    /* 2. Every page and tree node is already on the disk, in blocks the
     *    last commit does not use. Make sure they are really there. */
    disk_flush(disk);

    /* 3. Write the superblock into the slot the last commit did not use.
     *    Until the last block of it is written, mount still sees the old
     *    commit; after it, mount sees the new one. Never a mix. */
    const uint8_t *p = (const uint8_t *)&sb;
    for (uint32_t i = 0; i < SLOT_BLOCKS; i++)
        disk_write(disk, slot_start(sb.generation % 2) + i, p + i * BLOCK_SIZE);
    disk_flush(disk);

    /* 4. From now on, protect THIS state's blocks from reuse; the blocks
     *    only the previous commit needed become free. */
    alloc_commit(vol_allocator(v));
    return 0;
}

int rec_mount(Disk *disk, Volume **out)
{
    Superblock sb;

    if (!read_newest(disk, &sb))
        return -ENOENT; /* no commit on this disk */

    /* Rebuild the volume from the saved state; the reference counts are
     * recomputed by walking the trees. */
    Volume *v = vol_load(disk, sb.state, sb.state_len);
    if (!v)
        return -EIO;
    alloc_commit(vol_allocator(v)); /* what we just loaded is the commit */
    *out = v;
    return 0;
}

uint32_t rec_generation(Disk *disk)
{
    Superblock sb;
    return read_newest(disk, &sb) ? sb.generation : 0;
}
