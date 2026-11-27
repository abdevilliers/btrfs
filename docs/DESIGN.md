# toy-btrfs — design

## 1. Abstract disk (`core/disk.h`)

```c
typedef struct DiskOps {
    void (*read)(Disk *, uint32_t blk, void *buf);
    void (*write)(Disk *, uint32_t blk, const void *buf);
    void (*flush)(Disk *);
    void (*destroy)(Disk *);
} DiskOps;
struct Disk { const DiskOps *ops; uint32_t nblocks; unsigned long reads, writes; };
```

A concrete disk embeds `Disk` as its first member (C "inheritance") and
supplies the four functions. Two exist: `ramdisk_create()` (tests, demo;
keeps its contents after the Volume is dropped, which is how a crash is
simulated) and `filedisk_open()` (the shell; survives a real restart).

## 2. Allocator (`core/allocator.h`)

Two arrays of `uint32_t`, one entry per block:

| array | meaning |
|---|---|
| `refs[b]` | number of parents (tree slots or root entries) pointing at `b` now |
| `committed[b]` | `refs[b]` at the last commit |

`alloc_block` returns the first block with `refs == 0 && committed == 0`.
The second condition is the whole of crash safety: a block that the last
committed tree uses is never overwritten before the next commit.

## 3. Copy-on-write tree (`core/cowtree.h`)

```c
typedef struct { uint32_t child[4]; } Node;   /* one node per block, 0 = empty */
```

Four levels of nodes; level-1 nodes point at data blocks. Page `p` uses
child index `(p >> 2*(level-1)) & 3` at each level, giving 256 pages.

### node_cow — copy or not

| situation | action |
|---|---|
| block is new (0) | allocate an empty node |
| `refs == 1` and not committed | modify in place |
| `refs > 1` (shared with a checkpoint/other tree) | copy; every child +1; old node −1 |
| `refs == 1` but committed | same copy; old node drops to 0 so its children go −1 again: net "refs move" to the copy |

`cowtree_set` walks top-down (root first), applying `node_cow` at each
level, so by the time a child is examined its count already includes every
tree that can reach it. The old data block of the page is dropped.

`cowtree_drop(blk, level)` decrements a block; only if it reaches 0 does it
recurse into its children. Dropping a tree therefore stops at the first
shared block and frees exactly what only that tree used.

## 4. Volume (`core/volume.h`)

```c
typedef struct {
    uint32_t used, id, root, pad;
    uint64_t generation;
    char     name[32];
} RootEntry;                         /* one checkpoint */

typedef struct {
    Disk *disk; Allocator *alloc;
    CowTree  live;                   /* writable tree */
    RootEntry table[16];             /* root table */
    uint32_t next_id; uint64_t generation; uint32_t sb_slot;
} Volume;
```

`vol_write_page` always allocates a fresh data block, writes it, then
`cowtree_set`s the page. `vol_check` walks the live tree and all checkpoint
trees, counting each pointer once per *distinct* parent node, and compares
with `refs[]`.

## 5. Inodes and files (`core/fs.h`)

```c
typedef struct { uint32_t used, ino, size; char name[20]; } Inode;  /* 32 B */
```

Page 0 of every tree is the inode table (also the flat directory). File in
slot `i` stores page `k` at tree page `1 + 16*i + k`. Inode numbers start at
257 as in btrfs.

- `fs_write(v, name, data, len)` — creates the inode if needed, writes each
  4 KiB page **only if its contents changed**, clears pages past the new
  end, updates the size in the inode table (which is itself a COW write).
- `fs_read(tree, name, buf, cap)` — works on *any* tree, so the same code
  reads the live filesystem or a checkpoint.
- `fs_delete` — clears every page of the file and the inode.

Read path: superblock → live root → page 0 (inode table) → inode → data
pages → blocks (mirrors btrfs: superblock → root tree → FS tree → DIR_ITEM →
INODE_ITEM → EXTENT_DATA → disk).

## 6. Checkpointing (`features/checkpoint`)

| operation | steps | cost |
|---|---|---|
| `ckpt_create(name)` | new root entry, `root = live.root`, `refs[root]++`, `id = next_id++` | 0 blocks copied |
| write after | `node_cow` sees `refs > 1` on the path and copies it; old data block −1 (checkpoint keeps it) | only changed path |
| `ckpt_rollback(name, keep_newer)` | `refs[target]++`, drop old live tree, `live.root = target`; optionally delete checkpoints with larger id | frees only blocks the old live tree alone used |
| `ckpt_rollback_back(n, keep_newer)` | sort checkpoints by id, newest first; roll back to element `n-1` | — |
| `ckpt_delete(name)` | drop its tree, clear entry | frees only its private blocks |
| `ckpt_read(name)` | returns a read-only `CowTree` for `fs_read`/`fs_list` | — |

**Rolling back N checkpoints.** Because every checkpoint is an independent
root (not a diff against the previous one), rolling back to "3 back" is the
same cost as "1 back", and checkpoints newer than the target remain usable
(roll forward), unless `keep_newer = 0`.

Example (from `make demo`):

```
1 back = c4 : notes.txt = "version 4"
2 back = c3 : notes.txt = "version 3"
3 back = c2 : notes.txt = "version 2"   <- rollback 3 goes here
4 back = c1 : notes.txt = "version 1"
```

## 7. Duplication (`features/duplication`)

```
dup_copy_page(v, from, to):
    blk = cowtree_get(live, from)
    alloc_ref(blk)                 # "to" will hold a reference
    cowtree_set(live, to, blk)     # copies shared path, drops to's old block
```

`dup_copy_file(src, dst)` creates `dst`'s inode, calls `dup_copy_page` for
each page and sets the size. 0 data blocks copied. Writing either file later
uses a fresh block (data is never written in place), so the shared block's
count drops 2 → 1 and the other file still reads the original. Works with
checkpoints automatically: a block a checkpoint also shares simply has a
higher count.

## 8. Recovery (`features/recovery`)

```c
typedef struct {
    uint32_t magic, csum;       /* "BTRF", CRC32 of this struct */
    uint64_t generation;        /* commit number */
    uint32_t live_root;
    uint32_t slot;              /* 0/1: where table + refcounts are */
    uint32_t nblocks, block_size;
} Superblock;                   /* at block 1, mirror at block 2 */
```

**rec_commit**
1. New nodes/data are already on disk, only in blocks free in both `refs` and `committed`.
2. Write root table and refcount table into the spare slot (`slot ^ 1`).
3. Flush.
4. Write superblock (generation + 1), flush, write mirror, flush.
5. `committed = refs`.

**rec_mount**
1. Read both superblocks.
2. Keep those with correct magic and checksum; take the highest generation.
3. Load the root table and refcounts from its slot (`committed = refs`).
4. `live.root = sb.live_root` — the live tree and every checkpoint are back.

**Why a crash is safe at every point**

| crash during | result at mount |
|---|---|
| normal operation (before step 2) | old superblock → old slot → old tree, untouched because its blocks were never reused |
| step 2–3 | spare slot half written but no superblock points at it |
| step 4, primary torn | primary fails CRC, mirror (old gen) used |
| step 4, between primary and mirror | primary has the higher generation, used |

Tests cover: commit → change → drop volume → mount (state and checkpoints
back, `vol_check == 0`); corrupted primary → mirror; file disk across a real
close/reopen; blank disk → mount fails.
