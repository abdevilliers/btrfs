/*
 * ctree.h - PRIVATE to core/src: the in-memory structures and the functions
 * the core files share (the kernel's fs/btrfs/ctree.h, extent-tree.h, ...).
 *
 *   crc32c.c        checksums and name hashes
 *   extent_io.c     tree blocks in memory: cache, read + verify, write back
 *   ctree.c         the B-tree: search, copy-on-write, split, merge, insert, delete
 *   delayed_ref.c   reference count changes, queued and run at commit
 *   extent_tree.c   extent items and back-references, inc_ref / dec_ref,
 *                   tree block and data allocation, block groups, space
 *   free_space.c    in-memory free space + the free space tree
 *   transaction.c   start / end / abort / commit
 *   disk_io.c       superblocks, mkfs, mount, super-recover
 *   root_tree.c     root items, subvolumes, snapshots, drop_snapshot
 *   inode.c         inodes, directories, path lookup
 *   file.c          file data: read, write, truncate, clone, fiemap, csums
 *   check.c         btrfs check
 *   print_tree.c    dump-tree, dump-super
 */
#ifndef CTREE_H
#define CTREE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fs.h"

#define SECTORSIZE ((uint32_t)BLOCK_SIZE)

#define TRY(x)               \
    do {                     \
        int r_ = (x);        \
        if (r_ < 0)          \
            return r_;       \
    } while (0)

/* ------------------------------------------------------------- crc32c.c */

uint32_t crc32c(uint32_t seed, const void *data, size_t len); /* raw, no inversion */
uint32_t btrfs_csum(const void *data, size_t len);            /* ~crc32c(~0, ...)  */
uint64_t btrfs_name_hash(const char *name, size_t len);
uint64_t hash_extent_data_ref(uint64_t root, uint64_t owner, uint64_t offset);

/* -------------------------------------------- little-endian field access */

static inline uint64_t get64(const void *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static inline uint32_t get32(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static inline void put64(void *p, uint64_t v)
{
    memcpy(p, &v, 8);
}
static inline void put32(void *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

int btrfs_comp_keys(const BtrfsKey *a, const BtrfsKey *b);
static inline BtrfsKey mkkey(uint64_t objectid, uint8_t type, uint64_t offset)
{
    BtrfsKey k = { objectid, type, offset };
    return k;
}

/* ------------------------------------------------------- extent buffers */

/* One tree block in memory. */
typedef struct ExtentBuffer {
    uint64_t start;  /* bytenr                                   */
    uint8_t *data;   /* nodesize bytes                           */
    int dirty;       /* changed in this transaction: write it    */
    int stale;       /* freed and forgotten: never write it      */
    struct ExtentBuffer *hnext;
} ExtentBuffer;

/* Header field offsets (BtrfsHeader). */
#define HDR_FSID 32
#define HDR_BYTENR 48
#define HDR_FLAGS 56
#define HDR_CHUNK_UUID 64
#define HDR_GEN 80
#define HDR_OWNER 88
#define HDR_NRITEMS 96
#define HDR_LEVEL 100
#define HDR_SIZE 101
#define ITEM_SIZE 25
#define KEYPTR_SIZE 33

static inline uint64_t eb_bytenr(const ExtentBuffer *eb) { return get64(eb->data + HDR_BYTENR); }
static inline uint64_t eb_flags(const ExtentBuffer *eb) { return get64(eb->data + HDR_FLAGS); }
static inline uint64_t eb_gen(const ExtentBuffer *eb) { return get64(eb->data + HDR_GEN); }
static inline uint64_t eb_owner(const ExtentBuffer *eb) { return get64(eb->data + HDR_OWNER); }
static inline uint32_t eb_nritems(const ExtentBuffer *eb) { return get32(eb->data + HDR_NRITEMS); }
static inline int eb_level(const ExtentBuffer *eb) { return eb->data[HDR_LEVEL]; }
static inline void eb_set_bytenr(ExtentBuffer *eb, uint64_t v) { put64(eb->data + HDR_BYTENR, v); }
static inline void eb_set_flags(ExtentBuffer *eb, uint64_t v) { put64(eb->data + HDR_FLAGS, v); }
static inline void eb_set_gen(ExtentBuffer *eb, uint64_t v) { put64(eb->data + HDR_GEN, v); }
static inline void eb_set_owner(ExtentBuffer *eb, uint64_t v) { put64(eb->data + HDR_OWNER, v); }
static inline void eb_set_nritems(ExtentBuffer *eb, uint32_t v) { put32(eb->data + HDR_NRITEMS, v); }
static inline void eb_set_level(ExtentBuffer *eb, int v) { eb->data[HDR_LEVEL] = (uint8_t)v; }

void read_disk_key(const uint8_t *p, BtrfsKey *k);
void write_disk_key(uint8_t *p, const BtrfsKey *k);

/* Leaf items. */
void item_key(const ExtentBuffer *eb, int slot, BtrfsKey *k);
uint32_t item_offset(const ExtentBuffer *eb, int slot);
uint32_t item_size(const ExtentBuffer *eb, int slot);
uint8_t *item_ptr(const ExtentBuffer *eb, int slot);
void set_item_key(ExtentBuffer *eb, int slot, const BtrfsKey *k);
void set_item_offset(ExtentBuffer *eb, int slot, uint32_t v);
void set_item_size(ExtentBuffer *eb, int slot, uint32_t v);

/* Node pointers. */
void node_key(const ExtentBuffer *eb, int slot, BtrfsKey *k);
uint64_t node_blockptr(const ExtentBuffer *eb, int slot);
uint64_t node_ptr_gen(const ExtentBuffer *eb, int slot);
void set_node_key(ExtentBuffer *eb, int slot, const BtrfsKey *k);
void set_node_blockptr(ExtentBuffer *eb, int slot, uint64_t v);
void set_node_ptr_gen(ExtentBuffer *eb, int slot, uint64_t v);

/* ---------------------------------------------------------- free space */

typedef struct {
    uint64_t start, len;
} Range;

/* A sorted set of disjoint, merged byte ranges. */
typedef struct {
    Range *r;
    size_t n, cap;
} RangeSet;

int rs_add(RangeSet *s, uint64_t start, uint64_t len);    /* -ENOMEM */
int rs_remove(RangeSet *s, uint64_t start, uint64_t len); /* must be inside */
int rs_contains(const RangeSet *s, uint64_t start, uint64_t len);
uint64_t rs_total(const RangeSet *s);
void rs_free(RangeSet *s);

/* -------------------------------------------------------- block groups */

typedef struct BlockGroup {
    uint64_t start, len, flags; /* flags: BTRFS_BLOCK_GROUP_DATA or _METADATA */
    uint64_t used;              /* bytes in extent items (as of run refs)     */
    uint64_t reserved;          /* allocated, extent item not inserted yet    */
    uint64_t pinned;            /* freed, reusable after the commit           */
    uint64_t bytes_super;       /* superblock copies inside it                */
    RangeSet free;              /* the in-memory free space cache             */
    int dirty;                  /* its BLOCK_GROUP_ITEM needs updating        */
    int pending;                /* its BLOCK_GROUP_ITEM is not inserted yet   */
    int needs_free_space;       /* no FREE_SPACE_INFO in the tree yet         */
} BlockGroup;

enum { SPACE_DATA = 0, SPACE_METADATA = 1 };

typedef struct {
    uint64_t total, used, reserved, pinned, readonly, may_use;
} SpaceInfo;

/* ---------------------------------------------------------- delayed refs */

typedef struct {
    uint8_t type;    /* TREE_BLOCK_REF, SHARED_BLOCK_REF, EXTENT_DATA_REF, SHARED_DATA_REF */
    uint64_t root;   /* TREE / EXTENT_DATA: the root                                     */
    uint64_t parent; /* SHARED_*: the parent tree block                                  */
    uint64_t owner;  /* EXTENT_DATA: inode number                                        */
    uint64_t offset; /* EXTENT_DATA: file offset - extent offset                         */
    int64_t count;   /* + added, - dropped                                               */
} DelayedRef;

typedef struct DelayedHead {
    uint64_t bytenr, num_bytes;
    int is_data;
    int level;        /* tree blocks                                       */
    int must_insert;  /* allocated in this transaction: no extent item yet */
    uint64_t set_flags;
    int64_t ref_mod;  /* net change of the reference count                 */
    DelayedRef *refs;
    size_t nrefs, cap;
    struct DelayedHead *hnext;
} DelayedHead;

typedef struct {
    DelayedHead **buckets;
    size_t nbuckets, count;
} DelayedRefs;

/* ----------------------------------------------------------------- roots */

typedef struct Root {
    Btrfs *fs_info;
    uint64_t objectid;
    uint64_t key_offset;       /* ROOT_ITEM key offset (snapshot: creation transid) */
    BtrfsRootItem item;        /* in-memory copy of the root item                  */
    ExtentBuffer *node;        /* the current root block                           */
    uint64_t commit_root;      /* the root block at the last commit                */
    int shareable;             /* an FS tree: can be snapshotted                   */
    int dirty;                 /* the root item needs writing at commit            */
    int force_cow;             /* snapshotted in this commit: copy every block     */
    uint64_t highest_objectid; /* FS trees: highest inode number (0 = not known)   */
    struct Root *next;
} Root;

typedef struct PendingSnap {
    uint64_t src_id;                 /* subvolume to snapshot          */
    uint64_t parent_id, parent_dir;  /* where the new entry goes       */
    char name[FS_NAME_MAX + 1];
    int readonly;
    uint64_t new_id;                 /* filled in by the commit        */
    int error;
    struct PendingSnap *next;
} PendingSnap;

/* ---------------------------------------------------------- the fs_info */

struct Btrfs {
    Disk *disk;
    BtrfsSuper super;    /* the superblock as last committed       */
    uint32_t nodesize;
    uint8_t fsid[BTRFS_FSID_SIZE];
    uint64_t generation; /* the last committed transid             */
    uint64_t transid;    /* the running one: generation + 1        */
    int trans_dirty;     /* anything changed since the last commit */
    int error;           /* aborted: read-only from now on         */
    int read_only;
    int in_commit;
    int no_flush;        /* do not commit to find space (mkfs)     */
    int backup_index;

    Root *tree_root, *extent_root, *csum_root, *fst_root, *uuid_root;
    Root *roots;         /* every loaded root                      */

    ExtentBuffer **ebhash;
    size_t ebhash_n, eb_count;
    ExtentBuffer **dirty_ebs;
    size_t ndirty, cap_dirty;
    ExtentBuffer **stale_ebs;
    size_t nstale, cap_stale;

    BlockGroup **bgs;    /* sorted by start                        */
    size_t nbgs;
    SpaceInfo space[2];
    uint64_t global_rsv_size, global_rsv_left;

    DelayedRefs delayed;
    RangeSet pinned;     /* freed in this transaction              */
    PendingSnap *pending_snaps;
    int open_handles;
    uint64_t free_root_objectid; /* next subvolume id                */

    BtrfsStats stats;
};

struct Fs {
    Btrfs *b;
    uint64_t subvol; /* the subvolume this handle opened */
};

typedef struct {
    Btrfs *b;
    uint64_t rsv;   /* metadata bytes reserved for this operation  */
} Trans;

typedef struct {
    ExtentBuffer *nodes[BTRFS_MAX_LEVEL];
    int slots[BTRFS_MAX_LEVEL];
} Path;

static inline void path_init(Path *p) { memset(p, 0, sizeof *p); }

/* ----------------------------------------------------------- extent_io.c */

ExtentBuffer *eb_lookup(Btrfs *b, uint64_t bytenr);
ExtentBuffer *eb_new(Btrfs *b, uint64_t bytenr);   /* zeroed, replaces any cached one */
int read_tree_block(Btrfs *b, uint64_t bytenr, uint64_t gen, int level, ExtentBuffer **out);
void eb_mark_dirty(Btrfs *b, ExtentBuffer *eb);
void eb_forget(Btrfs *b, ExtentBuffer *eb);        /* freed: never write it        */
int write_dirty_tree_blocks(Btrfs *b);
void eb_free_stale(Btrfs *b);
void eb_cache_destroy(Btrfs *b);
int disk_read_bytes(Disk *d, uint64_t bytenr, void *buf, uint64_t len);
int disk_write_bytes(Disk *d, uint64_t bytenr, const void *buf, uint64_t len);
int verify_tree_block(const uint8_t *data, uint32_t nodesize, const uint8_t *fsid, uint64_t bytenr,
                      uint64_t gen, int level, char *why, size_t whylen);
void csum_tree_block(uint8_t *data, uint32_t nodesize);

/* --------------------------------------------------------------- ctree.c */

uint32_t leaf_data_size(const Btrfs *b);
uint32_t leaf_free_space(const Btrfs *b, const ExtentBuffer *leaf);
uint32_t max_node_ptrs(const Btrfs *b);

/* ins_len > 0: make room for an insert of that many bytes; < 0: about to
 * delete (merge small blocks on the way). cow: copy the path's blocks
 * into this transaction. Returns 0 if the key is there, 1 if not (the
 * slot is where it would go). */
int btrfs_search_slot(Trans *t, Root *root, const BtrfsKey *key, Path *p, int ins_len, int cow);
int btrfs_insert_empty_item(Trans *t, Root *root, Path *p, const BtrfsKey *key, uint32_t size);
int btrfs_insert_item(Trans *t, Root *root, const BtrfsKey *key, const void *data, uint32_t size);
int btrfs_del_item(Trans *t, Root *root, Path *p);  /* p from search(ins_len -1, cow 1) */
int btrfs_next_leaf(Btrfs *b, Path *p);               /* 1 = no more leaves */
int btrfs_next_item(Btrfs *b, Path *p);               /* 1 = no more items  */
int btrfs_prev_item(Btrfs *b, Path *p);               /* 1 = no earlier item */
void btrfs_extend_item(Trans *t, Path *p, uint32_t extra);   /* space checked by search */
void btrfs_truncate_item(Trans *t, Path *p, uint32_t new_size, int from_end);
void btrfs_set_item_key_safe(Trans *t, Path *p, const BtrfsKey *k);
void btrfs_mark_dirty(Trans *t, ExtentBuffer *eb);
/* Read-only search: the first item with key >= k (0) or none (1). */
int btrfs_search_forward(Btrfs *b, Root *root, const BtrfsKey *k, Path *p);
/* Item data, copied out. -ENOENT if the exact key is not there. */
int btrfs_lookup(Btrfs *b, Root *root, const BtrfsKey *k, void *buf, uint32_t len, uint32_t *size);
int btrfs_cow_root_node(Trans *t, Root *root);
int btrfs_update_item(Trans *t, Root *root, const BtrfsKey *k, const void *data, uint32_t len);
int btrfs_delete_key(Trans *t, Root *root, const BtrfsKey *k);
/* Read-only search positioned at the last item with key <= k (0), or 1. */
int btrfs_search_back(Btrfs *b, Root *root, const BtrfsKey *k, Path *p);

/* --------------------------------------------------------- delayed_ref.c */

int add_delayed_ref(Btrfs *b, uint64_t bytenr, uint64_t num_bytes, int is_data, int level,
                    const DelayedRef *ref, int must_insert);
int add_delayed_extent_op(Btrfs *b, uint64_t bytenr, uint64_t num_bytes, int is_data, int level,
                          uint64_t flags);
DelayedHead *find_delayed_head(Btrfs *b, uint64_t bytenr);
void remove_delayed_head(Btrfs *b, DelayedHead *h);
void free_delayed_head(DelayedHead *h);
int run_delayed_refs(Trans *t);
void delayed_refs_destroy(Btrfs *b);

/* --------------------------------------------------------- extent_tree.c */

int lookup_extent_info(Btrfs *b, uint64_t bytenr, int level, int is_data, uint64_t *refs,
                       uint64_t *flags);
int btrfs_inc_ref(Trans *t, Root *root, ExtentBuffer *eb, int full_backref);
int btrfs_dec_ref(Trans *t, Root *root, ExtentBuffer *eb, int full_backref);
int set_extent_flags(Trans *t, ExtentBuffer *eb, uint64_t flags);
int alloc_tree_block(Trans *t, Root *root, int level, ExtentBuffer **out);
int free_tree_block(Trans *t, uint64_t root_id, ExtentBuffer *eb, uint64_t parent, int last_ref);
int drop_tree_ref(Trans *t, uint64_t bytenr, int level, uint64_t root_id, uint64_t parent);
int add_data_ref(Trans *t, uint64_t bytenr, uint64_t num_bytes, uint64_t root, uint64_t ino,
                 uint64_t offset, int64_t delta, int must_insert);
int run_one_head(Trans *t, DelayedHead *h);

/* Data space: reserve bytes, then take extents out of the reservation. */
int reserve_data_space(Btrfs *b, uint64_t bytes);
void release_data_space(Btrfs *b, uint64_t bytes);
int alloc_data_extent(Btrfs *b, uint64_t want, uint64_t *start, uint64_t *len);
void unalloc_data_extent(Btrfs *b, uint64_t start, uint64_t len);

/* Metadata space. */
uint64_t metadata_bytes_for(const Btrfs *b, unsigned items);
int reserve_metadata(Btrfs *b, uint64_t bytes, int flush);
void release_metadata(Btrfs *b, uint64_t bytes);
void refill_global_rsv(Btrfs *b);

BlockGroup *find_block_group(Btrfs *b, uint64_t bytenr);
int alloc_chunk(Btrfs *b, uint64_t type_flags);
int load_block_groups(Btrfs *b);
int create_pending_block_groups(Trans *t);
int write_dirty_block_groups(Trans *t);
int any_dirty_block_groups(const Btrfs *b);
void unpin_extents(Btrfs *b);
void block_groups_destroy(Btrfs *b);
uint64_t total_bytes_used(const Btrfs *b);
int is_super_range(uint64_t start, uint64_t len, uint64_t disk_bytes);
uint64_t super_mirror_offset(int mirror);

/* ---------------------------------------------------------- free_space.c */

int fst_add(Trans *t, BlockGroup *bg, uint64_t start, uint64_t len);
int fst_remove(Trans *t, BlockGroup *bg, uint64_t start, uint64_t len);
int fst_add_block_group(Trans *t, BlockGroup *bg);
int fst_load_block_group(Btrfs *b, BlockGroup *bg);
void cache_add_free(Btrfs *b, BlockGroup *bg, uint64_t start, uint64_t len); /* minus supers */

/* --------------------------------------------------------- transaction.c */

/* Start an operation that changes up to `items` tree items: reserves the
 * metadata it could need (btrfs_start_transaction). */
int trans_start(Btrfs *b, Trans *t, unsigned items);
int trans_start_fallback_global(Btrfs *b, Trans *t, unsigned items); /* unlink, rmdir */
void trans_end(Trans *t);
int trans_abort(Trans *t, int err);   /* returns err */
int writable(const Btrfs *b);         /* 0, or -EROFS */

/* ------------------------------------------------------------ disk_io.c */

int write_all_supers(Btrfs *b);
void backup_super_roots(Btrfs *b);

/* ---------------------------------------------------------- root_tree.c */

Root *root_new(Btrfs *b, uint64_t objectid);
Root *get_root(Btrfs *b, uint64_t id);          /* NULL if none (or dropped) */
Root *get_fs_root(Btrfs *b, uint64_t id);       /* only live subvolumes      */
int load_root(Btrfs *b, Root *r);               /* from its root item        */
int update_root_item(Trans *t, Root *r);
int insert_root_item(Trans *t, Root *r);
int commit_fs_roots(Trans *t);
int commit_cowonly_roots(Trans *t, int *changed);
int create_pending_snapshots(Trans *t);
int drop_dead_roots(Btrfs *b);
void roots_destroy(Btrfs *b);
int create_empty_tree(Trans *t, uint64_t objectid, Root **out);
uint64_t highest_root_objectid(Btrfs *b);
int uuid_tree_add(Trans *t, const uint8_t *uuid, uint64_t id);
void init_subvol_item(Btrfs *b, BtrfsRootItem *it);
int find_root_ref(Btrfs *b, uint64_t subvol, uint64_t *parent, uint64_t *dirid, char *name);

/* -------------------------------------------------------------- inode.c */

/* An inode: which FS tree, which inode number. */
typedef struct {
    Root *root;
    uint64_t ino;
} Inode;

int inode_get(Btrfs *b, Inode in, BtrfsInodeItem *ii);
int inode_put(Trans *t, Inode in, const BtrfsInodeItem *ii);
void inode_init(BtrfsInodeItem *ii, uint64_t transid, uint32_t mode);
int resolve_path(Fs *fs, const char *path, Inode *out);
int resolve_parent(Fs *fs, const char *path, Inode *parent, char *name);
int dir_lookup(Btrfs *b, Inode dir, const char *name, BtrfsDiskKey *location, uint8_t *type);
int dir_add_entry(Trans *t, Inode dir, const char *name, const BtrfsKey *location, uint8_t type,
                  uint64_t *index);
int dir_del_entry(Trans *t, Inode dir, const char *name, uint64_t index);
int inode_ref_add(Trans *t, Inode in, uint64_t parent, const char *name, uint64_t index);
int inode_ref_del(Trans *t, Inode in, uint64_t parent, const char *name, uint64_t *index);
Root *fs_handle_root(Fs *fs);
void btrfs_random_uuid(uint8_t *u);
int read_range(Btrfs *b, Inode in, uint64_t off, uint8_t *buf, uint64_t len);
int make_root_dir(Trans *t, Root *root, uint64_t ino);
int fs_root_writable(Fs *fs, Root *root);
Root *subvol_entry_root(Btrfs *b, Root *root, uint64_t sub);
uint64_t now_sec(void);
int next_objectid(Btrfs *b, Root *root, uint64_t *ino);
int dir_is_empty(Btrfs *b, Inode dir);

/* --------------------------------------------------------------- file.c */

int drop_extents(Trans *t, Inode in, uint64_t start, uint64_t end, uint64_t *dropped_bytes);
int truncate_inode_items(Trans *t, Inode in, BtrfsInodeItem *ii, uint64_t new_size);
int csum_insert(Trans *t, uint64_t bytenr, const uint8_t *data, uint64_t len);
int csum_delete(Trans *t, uint64_t bytenr, uint64_t len);
int csum_lookup(Btrfs *b, uint64_t bytenr, uint32_t *csum);

#endif
