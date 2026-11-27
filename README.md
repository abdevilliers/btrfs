# toy-btrfs

A small, readable model of the **btrfs** copy-on-write filesystem in C11
(~650 lines of library code, ~1,500 including tests, demo and shell), with three advanced features:

| Feature | What it does | btrfs equivalent |
|---|---|---|
| **Checkpointing** | Named read-only copies of the whole filesystem; roll back **any number of checkpoints back**, or forward again | Snapshots |
| **Duplication** | Copy a file without copying its data (blocks are shared, split on write) | `cp --reflink` |
| **Recovery** | Commit in a crash-safe order; mount from the newest valid superblock (primary or mirror) | Transaction commit + superblock mirrors |

All three are built on one mechanism: **shared blocks with reference counts**.

## Build and run

Requires a C11 compiler and `make` (macOS or Linux).

```sh
make            # builds build/tests, build/demo, build/btrfs-shell
make test       # automated tests for core + every feature
make demo       # narrated walk-through of every feature
./build/demo --pause    # same, waits for Enter between steps (for recording)
make shell      # interactive shell on disk.img (persists between runs)
make clean
```

## Project layout

```
core/                      shared by every feature
  disk.h/.c                abstract Disk (method table) + RAM disk + file disk
  allocator.h/.c           free space + refcount per block      (extent tree)
  cowtree.h/.c             copy-on-write tree page -> block     (FS tree)
  volume.h/.c              live tree + root table of checkpoints (root tree)
  fs.h/.c                  inodes, file read/write/delete       (INODE/DIR/EXTENT items)
features/
  checkpoint/              ckpt_create/read/list/rollback/rollback_back/delete
  duplication/             dup_copy_page, dup_copy_file (reflink)
  recovery/                Superblock, rec_format, rec_commit, rec_mount
tools/btrfs-shell.c        interactive shell
demo/demo.c                narrated demo
tests/tests.c              automated tests
docs/DESIGN.md             data structures and algorithms in detail
```

Layers only call downward: `features → fs → volume → cowtree → allocator → disk`.

## On-disk layout

Block size 4 KiB, disk size 1024 blocks (4 MiB).

```
block 0      boot (unused)
block 1      superblock          magic, checksum, generation, live root, slot
block 2      superblock mirror
block 3,4    slot 0: root table, refcount table
block 5,6    slot 1: root table, refcount table   (commits alternate slots)
block 7 ...  tree nodes and data blocks
```

Inside every tree (the live filesystem and every checkpoint):

```
page 0              inode table (15 inodes, a flat root directory)
page 1 + i*16 + k   page k of the file in inode slot i   (max 64 KiB per file)
```

## Shell commands

```
ls | write <f> <text> | fill <f> <kb> <char> | cat <f> | rm <f> | blocks <f>
checkpoint <name> | checkpoints | rollback <N> | rollback-to <name>
rollback-drop <N> | ckpt-ls <name> | ckpt-cat <name> <f> | ckpt-rm <name>
cp <src> <dst>                                   (reflink copy)
commit | crash | corrupt-sb | exit               (exit = commit + quit)
info | check | format | help
```

`rollback N` makes the live filesystem equal to the checkpoint **N back**
(1 = newest). Newer checkpoints are kept so you can roll forward again;
`rollback-drop N` also deletes the newer ones.

## How each feature works (short version)

**Copy-on-write.** A tree node is changed in place only if exactly one
parent uses it *and* it is not part of the last commit. Otherwise it is
copied, each child gains a reference (the copy is a second parent), and
the old node loses one. Data is never overwritten: every write goes to a
new block.

**Checkpointing.** `ckpt_create` adds a root-table entry pointing at the
live root and increments the root's refcount — 0 blocks copied. Later
writes copy only the changed path. Rollback points the live tree at the
checkpoint's root (+1) and drops the old live tree, which frees only blocks
nothing else uses. Checkpoints are ordered by id, so any of them can be the
target.

**Duplication.** For each page: look up the source block, increment its
refcount, point the destination page at it. Writing either file later
gives that page a new block; the shared block's count drops back to 1.

**Recovery.** `rec_commit` writes the root table and refcounts to the
spare slot, flushes, writes the superblock (generation+1) then the mirror,
then records the counts as "committed". The allocator never reuses a block
the last commit uses, so the committed tree is intact whatever happens
afterwards. `rec_mount` validates both superblocks by CRC32 and loads the
newest.

`vol_check()` recomputes every refcount from the trees and compares; the
tests and demo assert it returns 0 after every scenario.

## Simplifications compared to real btrfs

- Fixed-shape radix tree (fanout 4, depth 4) instead of key-sorted B-trees with splitting.
- One flat directory, 15 files, 64 KiB per file; no permissions or timestamps.
- One device, no chunk tree / RAID / compression / data checksums.
- Refcounts kept as one table (written whole at commit) instead of an extent tree.
- Disk-full ends the program instead of returning ENOSPC.
