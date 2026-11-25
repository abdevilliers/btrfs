/*
 * crc32c.c - CRC-32C (Castagnoli), the checksum btrfs uses for tree
 * blocks, the superblock and data, and for name and backref hashes.
 */
#include "ctree.h"

static uint32_t table[256];
static int table_ready;

static void make_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        table[i] = c;
    }
    table_ready = 1;
}

/* The kernel's crc32c(seed, data, len): no inversion before or after. */
uint32_t crc32c(uint32_t seed, const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t c = seed;
    if (!table_ready)
        make_table();
    while (len--)
        c = table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c;
}

/* The "crc32c" shash used for checksums: start at ~0, invert at the end. */
uint32_t btrfs_csum(const void *data, size_t len)
{
    return ~crc32c(~0u, data, len);
}

/* btrfs_name_hash: the offset of a DIR_ITEM key. */
uint64_t btrfs_name_hash(const char *name, size_t len)
{
    return crc32c(~1u, name, len);
}

/* hash_extent_data_ref: the offset of an EXTENT_DATA_REF key. */
uint64_t hash_extent_data_ref(uint64_t root, uint64_t owner, uint64_t offset)
{
    uint32_t high = ~0u, low = ~0u;
    high = crc32c(high, &root, sizeof root);
    low = crc32c(low, &owner, sizeof owner);
    low = crc32c(low, &offset, sizeof offset);
    return ((uint64_t)high << 31) ^ (uint64_t)low;
}
