# How the filesystem works (`core/`)

This is a model of btrfs. The function names in the text are the Linux kernel's
(`fs/btrfs/…`), so each one can be looked up in the real source.

```sh
make test                                   # core/tests: ctree, fs, subvol, crash
make shell                                  # tools/fsh.c: try dump-tree 2 after a few writes
build/bin/fsh demo.img < tools/demo_session.txt
```

## 1. Everything is an item in a B-tree

btrfs stores no metadata at a fixed place, except the superblock. Every piece of
metadata is an **item**, found by its **key** `(objectid, type, offset)`. Keys are sorted
in that order. A **leaf** holds items; a **node** holds `(key, child block, child
generation)` pointers.

```text
leaf (nodesize bytes):
+--------+--------+--------+-----+-------------- free --------------+------+------+------+
| header | item 0 | item 1 | ... |                                  |data 2|data 1|data 0|
+--------+--------+--------+-----+----------------------------------+------+------+------+
  101 B    25 B each: key + offset + size         item data grows down from the end

header: csum (crc32c) | fsid | bytenr | flags | generation | owner (tree id) | nritems | level
```

The trees are listed below. Each one's root block is recorded in the root tree; the root
tree's own root block is in the superblock.

| Tree (id) | Its items |
|---|---|
| root tree (1) | `ROOT_ITEM` for every other tree; `ROOT_REF` / `ROOT_BACKREF` linking each subvolume to its directory; `ORPHAN_ITEM` for deleted subvolumes; the `default` directory item |
| extent tree (2) | an `EXTENT_ITEM` / `METADATA_ITEM` per allocated extent, with its back-references; a `BLOCK_GROUP_ITEM` per block group |
| FS tree (5, and 256+ for subvolumes) | per inode: `INODE_ITEM`, `INODE_REF`; per directory entry: `DIR_ITEM` + `DIR_INDEX`; per piece of file data: `EXTENT_DATA` |
| csum tree (7) | `EXTENT_CSUM`: the crc32c of every data sector |
| uuid tree (9) | subvolume uuid → id |
| free space tree (10) | per block group, `FREE_SPACE_INFO` and its free ranges |

`build/bin/fsh` → `dump-tree 2` prints a tree's items exactly as `btrfs inspect-internal
dump-tree` does.

## 2. Copy-on-write (`ctree.c`)

`btrfs_search_slot` walks from the root to a leaf. When the search is for a change, each
block on the way is copied first (`btrfs_cow_block`). The copy goes to a new place, and
the parent, itself already copied, is pointed at it. The committed tree is never
changed. The copies form a new tree, which shares every block off the path:

```text
before:            root A                    after a change under C:   root A'
                  /      \                                            /      \
                 B        C                                          B        C'
                / \      / \                                                 / \
               …   …    D   E                                               D   E'
```

A block copied in the running transaction, and not yet written, may be changed in place
(`should_cow_block`), so each block is copied at most once per transaction. On the way
down, a full node is split, and for a delete a near-empty node is merged with a
neighbour (`balance_level`). Leaves split where both halves fit, and merge after
deletes.

## 3. Who uses a block: references (`extent_tree.c`)

Every extent, whether a tree block or data, has an item with a reference count and one
**back-reference** item per user:

| Back-reference | Meaning |
|---|---|
| `TREE_BLOCK_REF (bytenr, root)` | a block owned by tree `root` points here |
| `SHARED_BLOCK_REF (bytenr, parent)` | tree block `parent` points here |
| `EXTENT_DATA_REF (bytenr, hash)` = `{root, inode, offset, count}` | file data of `inode` in `root` |
| `SHARED_DATA_REF (bytenr, parent)` = `{count}` | file data in leaf `parent` |

The plain ("implicit") refs name the **owner of the parent** rather than the parent. A
snapshot can then share a whole subtree while only the top of it gains a reference.
When a block is shared and then copied, `update_ref_for_cow` decides what changes:

| Block being copied | What happens |
|---|---|
| shared, owned by this tree, implicit refs | the block keeps its children through new SHARED refs (it gets `FULL_BACKREF`); the implicit refs now serve the copy |
| shared, otherwise | the copy gives each child a reference |
| only this tree uses it, `FULL_BACKREF` | the SHARED refs move to the copy |
| only this tree uses it | the copy takes over the children's references |

## 4. Delayed refs and pinned space (`delayed_ref.c`)

Reference changes are not made at once. Each "+1" or "−1" is queued in a head per extent,
and opposite changes cancel. The commit runs the queue: it inserts new extent items,
adds and removes back-references, and deletes extents whose count reaches 0. Running
them changes the extent tree, which queues more changes; the loop runs until nothing is
left.

An extent freed in a transaction is **pinned**: its space is not reused until the
transaction has committed, because the previous commit may still use it. One exception
is a tree block made and freed in the same transaction and never written: it is free at
once (`check_ref_cleanup`).

## 5. Space: block groups and the free space tree

Disk space is handed out in **block groups** (chunks), each `DATA` or `METADATA`:

```text
| 0 ... 1 MiB reserved (superblock at 64 KiB) | metadata bg | data bg | ... unallocated ... |
```

A new block group takes 10% of the disk (at least 1 MiB) when one is needed
(`btrfs_chunk_alloc`). The superblock copy at 64 MiB is excluded from any block group
holding it. Free space is whatever no extent item covers. The **free space tree** records
it per block group, so a mount reads it instead of scanning the extent tree. It is
updated when delayed refs run.

Before changing anything, an operation **reserves** the metadata it could need: per
item, enough to copy and split a path of the deepest tree (`nodesize × 8 × 2`). It
therefore fails with `-ENOSPC` before making a change. Writes reserve their data the
same way. If space is short, a commit is tried first, because it turns pinned space back
into free space. Deleting may use the **global reserve**, so a full disk can always be
emptied.

## 6. Files (`inode.c`, `file.c`)

```text
(257 INODE_ITEM 0)            size 20000, nlink 1, nbytes 20480, mode 0100644
(257 INODE_REF 256)           index 2, name "report.txt"       ← its name in dir 256
(256 DIR_ITEM crc32c(name))   location (257 INODE_ITEM 0)      ← lookup by name
(256 DIR_INDEX 2)             location (257 INODE_ITEM 0)      ← readdir order
(257 EXTENT_DATA 0)           disk_bytenr 7340032, num_bytes 20480, offset 0
```

- New inode numbers are the highest in the tree + 1. "." and ".." are not stored. A
  directory's size is twice the length of its names, and its nlink is 1.
- A file of up to 2048 bytes is **inline**: its data is inside the `EXTENT_DATA` item.
- A write never overwrites data. The new sectors go to a new extent, and
  `__btrfs_drop_extents` cuts the old items out of the range. An item that sticks out on
  both sides is split in two, and both halves reference the same extent (`count 2`). A
  gap is just a missing item (NO_HOLES).
- Every data sector's crc32c is stored in the csum tree and checked on read. A bad
  sector reads as `-EIO`.
- `fs_clone_range` (reflink) inserts items pointing at the source's extents, adding an
  `EXTENT_DATA_REF` for each. No data is read or written.

## 7. Subvolumes and snapshots (`root_tree.c`)

A **subvolume** is an FS tree of its own. Its `ROOT_ITEM` is in the root tree, and a
directory entry `(id, ROOT_ITEM, -1)` with a `ROOT_REF` / `ROOT_BACKREF` pair gives it a
name. `fs_open` opens the **default** subvolume, which starts as 5.

A **snapshot** is made inside the commit (`create_pending_snapshot`):

1. The source's root block is copied once more. The copy belongs to the new subvolume,
   and each of its children gains a reference from it (`btrfs_copy_root` +
   `btrfs_inc_ref`).
2. The source's `last_snapshot` becomes this transaction. Blocks at least that old may be
   shared, so copy-on-write must look up their reference counts.
3. Until the commit ends, every source block is copied before a change (`FORCE_COW`).

**Deleting** a subvolume removes its name, sets its refs to 0 and adds an
`ORPHAN_ITEM`. After the commit, the cleaner (`btrfs_drop_snapshot`) walks the tree. For
each block, it drops this tree's reference. A block only this tree uses has its
children's references dropped first. A shared block owned by this tree is first
switched to SHARED refs (the `UPDATE_BACKREF` stage), because implicit refs naming a dead
tree could never be matched by a later copy-on-write.

## 8. Transactions, the superblock and recovery

```text
commit (btrfs_commit_transaction):
  create pending block groups; run delayed refs
  make pending snapshots; write changed subvolumes' root items
  repeat: run delayed refs, write block group items, write the extent/csum/free space/uuid
          trees' root items                                   (until nothing changes)
  write every new tree block, with its checksum; flush
  write the superblock to every copy (64 KiB, 64 MiB, 256 GiB); flush   ← durable here
  pinned space becomes free; the cleaner runs
```

A crash before the superblock write leaves the previous commit. Nothing it uses was
overwritten. `core/tests/test_crash.c` cuts the power after every single write of a
commit and checks this.

Each superblock keeps the **backup roots** of the last four commits. If the newest root
tree is damaged, `mount -o usebackuproot` (`BtrfsMountOpts.usebackuproot`) uses the newest
backup that reads cleanly. Like the kernel, mount reads only the primary superblock;
`btrfs_super_recover` (btrfs rescue super-recover) copies a good mirror over a damaged
primary.

Every tree block read from the disk is verified against four things:

- its checksum;
- its own bytenr and the fsid;
- the generation its parent's pointer expects, which catches a write that was lost or
  went to the wrong place;
- the level expected.

## 9. btrfs check (`check.c`)

`btrfs_check(disk)` mounts the committed state read-only and checks everything from
scratch. The tests run it after nearly every commit.

1. **Tree blocks:** each one's checksum, bytenr, fsid, generation and level, its key
   order and item bounds, and that each node key equals its child's first key.
2. **References:** walking every tree gives the back-references each extent should have.
   They must equal the extent tree's exactly, and each `refs` must be their sum. Nothing
   may be leaked or overlap.
3. **Block groups:** each extent is in a block group of the right type, `used` adds up,
   and so does the superblock's `bytes_used`.
4. **Free space tree:** each block group minus its extents, exactly.
5. **csum tree:** it covers exactly the data extents, and every data sector matches its
   checksum (a scrub).
6. **Subvolumes:** nlink against `INODE_REF`s; `DIR_ITEM`, `DIR_INDEX` and `INODE_REF`
   agree; directory sizes; file extents are sorted, don't overlap, follow the inline
   rules and add up to `nbytes`; `ROOT_REF`, `ROOT_BACKREF`, orphans and the uuid tree
   are consistent.

## 10. Limits

| Limit | Value |
|---|---|
| Sector | 4096 bytes |
| Tree block (nodesize) | 4096 – 65536 (mkfs default 16384; the tests use 4096) |
| Smallest disk | 4 MiB |
| Inline file data | 2048 bytes |
| Name / path | 255 / 4095 bytes |
| Tree levels | 8 |

For what is not modelled, see the end of [`README.md`](../README.md).
