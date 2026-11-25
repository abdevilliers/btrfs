# btrfs: a working model of the btrfs filesystem, in C

The **core** in `core/` is a working model of btrfs. It uses the btrfs on-disk format, the
same trees and the same algorithms as the Linux kernel's `fs/btrfs`, in about 6,700
readable lines. Each team member builds one **feature** on top of it, in its own folder.

| Part | Folder | Status |
|---|---|---|
| The filesystem: copy-on-write B-trees, extent tree with back-references, delayed refs, block groups, free space tree, checksums, transactions, subvolumes, snapshots, reflinks, `btrfs check` | `core/` (`fs.h`, see [`core/FILESYSTEM.md`](core/FILESYSTEM.md)) | Done: tests, a shell |
| Checkpointing: read-only snapshots, rollback by name or "go back x", timed | `features/checkpoint/` | Done: tests and demos |
| Duplication: reflink copies (`cp --reflink`) | `features/duplication/` | Done: tests and demo |
| Recovery: commit, crash, mount, backup roots, super-recover | `features/recovery/` | Done: tests and demo |
| Backup (send/receive) | `features/backup/` | To do |

## Build and run

```sh
make          # build everything into build/
make test     # core tests, then every feature's tests
make demo     # every feature's demo
make shell    # an interactive shell on fsh.img (tools/fsh.c)
make clean
```

This needs only a C11 compiler (clang on macOS, gcc on Linux) and `make`. The build turns
on strict warnings (`-Wconversion`, `-Wshadow`, …) and compiles with none. The on-disk
structures are little-endian, so the code needs a little-endian machine (x86-64, arm64).

## Layout

```text
btrfs/
├── Makefile                  finds every feature automatically
├── core/                     SHARED by all features: change with care
│   ├── include/              public headers: the only ones features may include
│   │   ├── fs.h              the API: mkfs, mount, commit, files, subvolumes, check
│   │   ├── btrfs_tree.h      the on-disk format (keys, item types, structures)
│   │   ├── disk.h            Disk: a simulated disk of 4 KiB sectors
│   │   ├── mem_disk.h        a Disk in RAM
│   │   ├── file_disk.h       a Disk in a file (survives the program)
│   │   └── fault_disk.h      a Disk that loses power after n writes (crash tests)
│   ├── src/                  the implementation; ctree.h is private
│   │   ├── ctree.c           the B-tree: search, copy-on-write, split, merge
│   │   ├── extent_tree.c     extent items, back-references, allocation, block groups
│   │   ├── delayed_ref.c     reference count changes, queued until commit
│   │   ├── free_space.c      free space in memory and in the free space tree
│   │   ├── transaction.c     start, abort, commit
│   │   ├── disk_io.c         superblocks, mkfs, mount, super-recover
│   │   ├── root_tree.c       subvolumes, snapshots, drop_snapshot
│   │   ├── inode.c           inodes, directories, path lookup
│   │   ├── file.c            file data: read, write, truncate, reflink, fiemap
│   │   ├── check.c           btrfs check
│   │   └── print_tree.c      dump-tree, dump-super
│   ├── tests/                test_ctree, test_fs, test_subvol, test_crash
│   └── FILESYSTEM.md         how it works
├── tools/fsh.c               the shell (and demo_session.txt)
└── features/
    ├── checkpoint/           include/ src/ tests/ demo/ README.md
    ├── duplication/          same shape
    ├── recovery/             same shape
    └── backup/               README.md: where to start
```

```text
   checkpoint   duplication   recovery   backup      features (one owner each)
        \           |            |         /
         └──────────── fs.h ──────────────┘          core: the public API
                         │
   subvolumes, files  (root_tree.c, inode.c, file.c)
                         │
   transactions  (transaction.c, disk_io.c)
                         │
   B-trees + extent tree + delayed refs + free space  (ctree.c, extent_tree.c, ...)
                         │
                  disk.h ← mem_disk.h, file_disk.h, fault_disk.h
```

## Rules that keep features independent

1. **Core never includes a feature.** Dependencies point one way only: features → core.
2. **Features include only `core/include/*.h`**, and other features' `include/` if they
   really need them. The Makefile builds features *without* access to `core/src`.
   Core's own tests may use `core/src/ctree.h`.
3. **Each handle is opaque.** `Btrfs` (a mounted filesystem) and `Fs` (one subvolume of
   it) are declared in `fs.h`; their layout is private.
4. **`btrfs_check(disk)` must stay clean.** Commit, then check: it rebuilds every
   reference count from the trees and compares it with the extent tree. Every test does
   this.
5. **Errors are negative errno values** (`-ENOENT`, `-EEXIST`, `-ENOSPC`, `-EROFS`, …).
   0 means success. An error after a change has started aborts the transaction: the
   filesystem turns read-only, and the disk keeps the last commit.
6. **A change to core is a change for everyone.** Run `make test` before pushing.

## What core gives a feature

| Need | Core API (`fs.h`) |
|---|---|
| Make, mount, commit, unmount, crash | `btrfs_mkfs`, `btrfs_mount`, `btrfs_commit`, `btrfs_unmount`, `btrfs_crash` |
| Files and directories | `fs_create`, `fs_mkdir`, `fs_write`, `fs_read`, `fs_truncate`, `fs_unlink`, `fs_rmdir`, `fs_link`, `fs_stat`, `fs_readdir` |
| Subvolumes and snapshots | `fs_subvol_create`, `fs_subvol_snapshot`, `fs_subvol_delete`, `fs_subvol_list`, `fs_subvol_info`, `fs_set_default_subvol` |
| Open a subvolume | `fs_open` (the default one), `fs_open_subvol` |
| Share data between files | `fs_clone_range` (FICLONERANGE) |
| Where a file's data is | `fs_fiemap` |
| Space | `fs_space` |
| Read any tree's items (send/receive, inspection) | `btrfs_tree_search`, `btrfs_dump_tree`, `btrfs_dump_super` |
| Check everything | `btrfs_check` |
| Recover | `btrfs_mount` with `usebackuproot`, `btrfs_super_recover` |
| Simulate a power cut | `fault_disk_wrap`, `fault_disk_cut_after` |

## How close is this to real btrfs?

It uses the btrfs on-disk structures (`btrfs_tree.h`), byte for byte. The main
mechanisms are btrfs's own:

- keyed B-trees that are copied on write;
- the root, extent, FS, csum, free space and uuid trees;
- implicit and full back-references;
- `update_ref_for_cow` and `btrfs_drop_snapshot`;
- delayed refs, pinned extents, block groups and the global reserve;
- metadata reservations;
- inline extents, `__btrfs_drop_extents`, NO_HOLES and crc32c data checksums;
- superblock mirrors with backup roots;
- subvolumes, snapshots made during commit, and the cleaner.

These parts are left out or simplified:

- **One device only.** There is no chunk tree, no device tree and no RAID or DUP
  profiles. Logical addresses are physical. The superblock's `sys_chunk_array` is empty.
- **No compression, log tree (fsync), quotas, xattrs, rename, permissions or
  relocation.** Timestamps are kept, at whole seconds.
- **Writes go straight to the disk.** btrfs buffers dirty pages and allocates when it
  writes them back (delalloc). Commits happen when asked, when a snapshot is taken, or
  when space is short. btrfs also commits every 30 seconds.
- **Back-references are always separate items.** btrfs normally stores most of them
  inline, inside the extent item. Both forms are valid btrfs.
- **Leaves are not "pushed" into neighbours before a split**, and the free space tree
  never uses bitmaps. Data allocation is first-fit, with no clusters.
- **A snapshot does not show the subvolumes nested in its source.** btrfs shows them as
  empty directories; here they are skipped.
- **Single-threaded:** there are no locks, no worker threads and no cleaner thread. The
  cleaner runs after each commit.
