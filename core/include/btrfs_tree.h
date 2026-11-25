/*
 * btrfs_tree.h - the on-disk format, as in Linux's
 * include/uapi/linux/btrfs_tree.h.
 *
 * Everything btrfs stores is an ITEM in a B-tree. An item is found by its
 * KEY (objectid, type, offset), and keys are sorted in that order. Leaves
 * hold items; nodes hold (key, child block, child generation) pointers.
 *
 * The structures below are byte-for-byte the btrfs layouts (little-endian,
 * packed). This code assumes a little-endian host (x86-64, arm64).
 */
#ifndef BTRFS_TREE_H
#define BTRFS_TREE_H

#include <stdint.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the on-disk structures are little-endian; this code needs a little-endian host"
#endif

#define BTRFS_PACKED __attribute__((packed))

/* ------------------------------------------------------------ objectids */

#define BTRFS_ROOT_TREE_OBJECTID 1ULL        /* the tree of tree roots        */
#define BTRFS_EXTENT_TREE_OBJECTID 2ULL      /* allocated extents + backrefs  */
#define BTRFS_CHUNK_TREE_OBJECTID 3ULL       /* (not modelled: one device)    */
#define BTRFS_DEV_TREE_OBJECTID 4ULL         /* (not modelled: one device)    */
#define BTRFS_FS_TREE_OBJECTID 5ULL          /* the top-level subvolume       */
#define BTRFS_ROOT_TREE_DIR_OBJECTID 6ULL    /* holds the "default" dir item  */
#define BTRFS_CSUM_TREE_OBJECTID 7ULL        /* data checksums                */
#define BTRFS_UUID_TREE_OBJECTID 9ULL        /* subvolume uuid -> id          */
#define BTRFS_FREE_SPACE_TREE_OBJECTID 10ULL /* free space per block group    */

#define BTRFS_ORPHAN_OBJECTID ((uint64_t)-5)
#define BTRFS_EXTENT_CSUM_OBJECTID ((uint64_t)-10)

#define BTRFS_FIRST_FREE_OBJECTID 256ULL /* first inode / subvolume id */
#define BTRFS_LAST_FREE_OBJECTID ((uint64_t)-256)

/* ----------------------------------------------------------- item types */

#define BTRFS_INODE_ITEM_KEY 1
#define BTRFS_INODE_REF_KEY 12
#define BTRFS_ORPHAN_ITEM_KEY 48
#define BTRFS_DIR_ITEM_KEY 84
#define BTRFS_DIR_INDEX_KEY 96
#define BTRFS_EXTENT_DATA_KEY 108
#define BTRFS_EXTENT_CSUM_KEY 128
#define BTRFS_ROOT_ITEM_KEY 132
#define BTRFS_ROOT_BACKREF_KEY 144
#define BTRFS_ROOT_REF_KEY 156
#define BTRFS_EXTENT_ITEM_KEY 168
#define BTRFS_METADATA_ITEM_KEY 169
#define BTRFS_TREE_BLOCK_REF_KEY 176
#define BTRFS_EXTENT_DATA_REF_KEY 178
#define BTRFS_SHARED_BLOCK_REF_KEY 182
#define BTRFS_SHARED_DATA_REF_KEY 184
#define BTRFS_BLOCK_GROUP_ITEM_KEY 192
#define BTRFS_FREE_SPACE_INFO_KEY 198
#define BTRFS_FREE_SPACE_EXTENT_KEY 199
#define BTRFS_UUID_KEY_SUBVOL 251

/* ------------------------------------------------------------- limits */

#define BTRFS_MAX_LEVEL 8
#define BTRFS_CSUM_SIZE 32
#define BTRFS_FSID_SIZE 16
#define BTRFS_UUID_SIZE 16
#define BTRFS_NAME_LEN 255
#define BTRFS_LABEL_SIZE 256
#define BTRFS_NUM_BACKUP_ROOTS 4
#define BTRFS_SUPER_INFO_SIZE 4096
#define BTRFS_MAGIC 0x4D5F53665248425FULL /* "_BHRfS_M" */

/* Superblock copies: 64 KiB, 64 MiB, 256 GiB (those inside the disk). */
#define BTRFS_SUPER_MIRROR_MAX 3
#define BTRFS_SUPER_INFO_OFFSET (64ULL * 1024)

/* --------------------------------------------------------------- keys */

typedef struct BTRFS_PACKED {
    uint64_t objectid;
    uint8_t type;
    uint64_t offset;
} BtrfsDiskKey;

/* The same key in CPU order (identical on a little-endian host). */
typedef struct {
    uint64_t objectid;
    uint8_t type;
    uint64_t offset;
} BtrfsKey;

/* ---------------------------------------------------------- tree blocks */

/* header flags */
#define BTRFS_HEADER_FLAG_WRITTEN (1ULL << 0)
#define BTRFS_HEADER_FLAG_RELOC (1ULL << 1)
#define BTRFS_MIXED_BACKREF_REV 1ULL
#define BTRFS_BACKREF_REV_SHIFT 56

/* Every tree block starts with this (101 bytes). */
typedef struct BTRFS_PACKED {
    uint8_t csum[BTRFS_CSUM_SIZE];           /* crc32c of everything after it */
    uint8_t fsid[BTRFS_FSID_SIZE];           /* which filesystem               */
    uint64_t bytenr;                         /* where this block should be     */
    uint64_t flags;                          /* WRITTEN, backref revision      */
    uint8_t chunk_tree_uuid[BTRFS_UUID_SIZE];
    uint64_t generation;                     /* transaction that wrote it      */
    uint64_t owner;                          /* tree that created it           */
    uint32_t nritems;
    uint8_t level;                           /* 0 = leaf                       */
} BtrfsHeader;

/* A leaf: header, items growing up, item data growing down from the end. */
typedef struct BTRFS_PACKED {
    BtrfsDiskKey key;
    uint32_t offset; /* of the data, from the end of the header */
    uint32_t size;
} BtrfsItem;

/* A node: header, then key pointers. */
typedef struct BTRFS_PACKED {
    BtrfsDiskKey key;
    uint64_t blockptr;   /* child block                          */
    uint64_t generation; /* the generation the child must have   */
} BtrfsKeyPtr;

/* ------------------------------------------------------ the FS trees */

typedef struct BTRFS_PACKED {
    uint64_t sec;
    uint32_t nsec;
} BtrfsTimespec;

typedef struct BTRFS_PACKED {
    uint64_t generation; /* transid that created it  */
    uint64_t transid;    /* transid that last changed it */
    uint64_t size;
    uint64_t nbytes;     /* bytes of extents it uses */
    uint64_t block_group;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint64_t rdev;
    uint64_t flags;
    uint64_t sequence;
    uint64_t reserved[4];
    BtrfsTimespec atime;
    BtrfsTimespec ctime;
    BtrfsTimespec mtime;
    BtrfsTimespec otime;
} BtrfsInodeItem;

/* INODE_REF (inode, 12, parent dir): one or more of these, each followed by
 * its name. */
typedef struct BTRFS_PACKED {
    uint64_t index; /* the DIR_INDEX of this name */
    uint16_t name_len;
} BtrfsInodeRef;

/* DIR_ITEM (dir, 84, name hash) and DIR_INDEX (dir, 96, index): each is
 * followed by its name. A DIR_ITEM can hold several (hash collisions). */
typedef struct BTRFS_PACKED {
    BtrfsDiskKey location; /* (ino, INODE_ITEM, 0) or (subvol, ROOT_ITEM, -1) */
    uint64_t transid;
    uint16_t data_len;
    uint16_t name_len;
    uint8_t type;          /* BTRFS_FT_* */
} BtrfsDirItem;

#define BTRFS_FT_REG_FILE 1
#define BTRFS_FT_DIR 2

#define BTRFS_FILE_EXTENT_INLINE 0
#define BTRFS_FILE_EXTENT_REG 1

/* EXTENT_DATA (inode, 108, file offset). An inline extent stops after
 * `type` and the file data follows. */
typedef struct BTRFS_PACKED {
    uint64_t generation;
    uint64_t ram_bytes;
    uint8_t compression;
    uint8_t encryption;
    uint16_t other_encoding;
    uint8_t type;
    uint64_t disk_bytenr;    /* the data extent (0 = hole)            */
    uint64_t disk_num_bytes; /* its size                              */
    uint64_t offset;         /* where in the extent this item starts  */
    uint64_t num_bytes;      /* bytes of the file this item covers    */
} BtrfsFileExtentItem;

#define BTRFS_FILE_EXTENT_INLINE_DATA_START 21 /* offsetof(..., disk_bytenr) */

/* ------------------------------------------------------- the root tree */

#define BTRFS_ROOT_SUBVOL_RDONLY (1ULL << 0)

typedef struct BTRFS_PACKED {
    BtrfsInodeItem inode;
    uint64_t generation;
    uint64_t root_dirid;
    uint64_t bytenr;        /* the tree's root block            */
    uint64_t byte_limit;
    uint64_t bytes_used;
    uint64_t last_snapshot; /* transid of the last snapshot     */
    uint64_t flags;
    uint32_t refs;          /* 0 = deleted, waiting to be dropped */
    BtrfsDiskKey drop_progress;
    uint8_t drop_level;
    uint8_t level;
    uint64_t generation_v2;
    uint8_t uuid[BTRFS_UUID_SIZE];
    uint8_t parent_uuid[BTRFS_UUID_SIZE];
    uint8_t received_uuid[BTRFS_UUID_SIZE];
    uint64_t ctransid;
    uint64_t otransid;
    uint64_t stransid;
    uint64_t rtransid;
    BtrfsTimespec ctime;
    BtrfsTimespec otime;
    BtrfsTimespec stime;
    BtrfsTimespec rtime;
    uint64_t reserved[8];
} BtrfsRootItem;

/* ROOT_REF (parent subvol, 156, child) and ROOT_BACKREF (child, 144,
 * parent), each followed by the name. */
typedef struct BTRFS_PACKED {
    uint64_t dirid;
    uint64_t sequence;
    uint16_t name_len;
} BtrfsRootRef;

/* ----------------------------------------------------- the extent tree */

#define BTRFS_EXTENT_FLAG_DATA (1ULL << 0)
#define BTRFS_EXTENT_FLAG_TREE_BLOCK (1ULL << 1)
#define BTRFS_BLOCK_FLAG_FULL_BACKREF (1ULL << 8)

/* EXTENT_ITEM (bytenr, 168, length) for data, METADATA_ITEM (bytenr, 169,
 * level) for tree blocks ("skinny metadata"). */
typedef struct BTRFS_PACKED {
    uint64_t refs;
    uint64_t generation;
    uint64_t flags;
} BtrfsExtentItem;

/* EXTENT_DATA_REF (bytenr, 178, hash(root, objectid, offset)) */
typedef struct BTRFS_PACKED {
    uint64_t root;
    uint64_t objectid;
    uint64_t offset;
    uint32_t count;
} BtrfsExtentDataRef;

/* SHARED_DATA_REF (bytenr, 184, parent leaf) */
typedef struct BTRFS_PACKED {
    uint32_t count;
} BtrfsSharedDataRef;

#define BTRFS_BLOCK_GROUP_DATA (1ULL << 0)
#define BTRFS_BLOCK_GROUP_SYSTEM (1ULL << 1)
#define BTRFS_BLOCK_GROUP_METADATA (1ULL << 2)

/* BLOCK_GROUP_ITEM (start, 192, length) */
typedef struct BTRFS_PACKED {
    uint64_t used;
    uint64_t chunk_objectid;
    uint64_t flags;
} BtrfsBlockGroupItem;

/* FREE_SPACE_INFO (block group start, 198, length); the free extents are
 * FREE_SPACE_EXTENT (start, 199, length) items with no data. */
typedef struct BTRFS_PACKED {
    uint32_t extent_count;
    uint32_t flags;
} BtrfsFreeSpaceInfo;

/* -------------------------------------------------------- the superblock */

typedef struct BTRFS_PACKED {
    uint64_t devid;
    uint64_t total_bytes;
    uint64_t bytes_used;
    uint32_t io_align;
    uint32_t io_width;
    uint32_t sector_size;
    uint64_t type;
    uint64_t generation;
    uint64_t start_offset;
    uint32_t dev_group;
    uint8_t seek_speed;
    uint8_t bandwidth;
    uint8_t uuid[BTRFS_UUID_SIZE];
    uint8_t fsid[BTRFS_FSID_SIZE];
} BtrfsDevItem;

typedef struct BTRFS_PACKED {
    uint64_t tree_root;
    uint64_t tree_root_gen;
    uint64_t chunk_root;
    uint64_t chunk_root_gen;
    uint64_t extent_root;
    uint64_t extent_root_gen;
    uint64_t fs_root;
    uint64_t fs_root_gen;
    uint64_t dev_root;
    uint64_t dev_root_gen;
    uint64_t csum_root;
    uint64_t csum_root_gen;
    uint64_t total_bytes;
    uint64_t bytes_used;
    uint64_t num_devices;
    uint64_t unused_64[4];
    uint8_t tree_root_level;
    uint8_t chunk_root_level;
    uint8_t extent_root_level;
    uint8_t fs_root_level;
    uint8_t dev_root_level;
    uint8_t csum_root_level;
    uint8_t unused_8[10];
} BtrfsRootBackup;

#define BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE (1ULL << 0)
#define BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID (1ULL << 1)
#define BTRFS_FEATURE_INCOMPAT_MIXED_BACKREF (1ULL << 0)
#define BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA (1ULL << 8)
#define BTRFS_FEATURE_INCOMPAT_NO_HOLES (1ULL << 9)
#define BTRFS_CSUM_TYPE_CRC32 0

typedef struct BTRFS_PACKED {
    uint8_t csum[BTRFS_CSUM_SIZE];
    uint8_t fsid[BTRFS_FSID_SIZE];
    uint64_t bytenr;      /* where this copy is */
    uint64_t flags;
    uint64_t magic;
    uint64_t generation;  /* the last committed transaction */
    uint64_t root;        /* the root tree's root block     */
    uint64_t chunk_root;
    uint64_t log_root;
    uint64_t unused_log_root_transid;
    uint64_t total_bytes;
    uint64_t bytes_used;
    uint64_t root_dir_objectid;
    uint64_t num_devices;
    uint32_t sectorsize;
    uint32_t nodesize;
    uint32_t unused_leafsize;
    uint32_t stripesize;
    uint32_t sys_chunk_array_size;
    uint64_t chunk_root_generation;
    uint64_t compat_flags;
    uint64_t compat_ro_flags;
    uint64_t incompat_flags;
    uint16_t csum_type;
    uint8_t root_level;
    uint8_t chunk_root_level;
    uint8_t log_root_level;
    BtrfsDevItem dev_item;
    char label[BTRFS_LABEL_SIZE];
    uint64_t cache_generation;
    uint64_t uuid_tree_generation;
    uint8_t metadata_uuid[BTRFS_FSID_SIZE];
    uint64_t nr_global_roots;
    uint64_t reserved[27];
    uint8_t sys_chunk_array[2048];
    BtrfsRootBackup super_roots[BTRFS_NUM_BACKUP_ROOTS];
    uint8_t padding[565];
} BtrfsSuper;

_Static_assert(sizeof(BtrfsDiskKey) == 17, "btrfs_disk_key");
_Static_assert(sizeof(BtrfsHeader) == 101, "btrfs_header");
_Static_assert(sizeof(BtrfsItem) == 25, "btrfs_item");
_Static_assert(sizeof(BtrfsKeyPtr) == 33, "btrfs_key_ptr");
_Static_assert(sizeof(BtrfsInodeItem) == 160, "btrfs_inode_item");
_Static_assert(sizeof(BtrfsInodeRef) == 10, "btrfs_inode_ref");
_Static_assert(sizeof(BtrfsDirItem) == 30, "btrfs_dir_item");
_Static_assert(sizeof(BtrfsFileExtentItem) == 53, "btrfs_file_extent_item");
_Static_assert(sizeof(BtrfsRootItem) == 439, "btrfs_root_item");
_Static_assert(sizeof(BtrfsRootRef) == 18, "btrfs_root_ref");
_Static_assert(sizeof(BtrfsExtentItem) == 24, "btrfs_extent_item");
_Static_assert(sizeof(BtrfsExtentDataRef) == 28, "btrfs_extent_data_ref");
_Static_assert(sizeof(BtrfsBlockGroupItem) == 24, "btrfs_block_group_item");
_Static_assert(sizeof(BtrfsDevItem) == 98, "btrfs_dev_item");
_Static_assert(sizeof(BtrfsRootBackup) == 168, "btrfs_root_backup");
_Static_assert(sizeof(BtrfsSuper) == BTRFS_SUPER_INFO_SIZE, "btrfs_super_block");

#endif
