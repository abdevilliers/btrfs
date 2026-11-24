# btrfs: a btrfs-style copy-on-write storage project

This is a small, readable model of how btrfs stores data, built in C as abstract
data types. A shared **core** provides the copy-on-write tree, reference counts, and
a Unix-style **filesystem** stored inside it: inodes, directories and files, with
Bach's `ialloc` / `ifree` (free inode list) and `alloc` / `free` (chained free block
list). The advanced features run on top of normal file reads and writes. Each team member builds one **feature** on top of it, in its own folder.

| Part | Folder | Status |
|---|---|---|
| Filesystem: files, directories, `ialloc`/`ifree`, `alloc`/`free`, `namei`, `bmap`, hard links, fsck | `core/` (`fs.h`, see [`core/FILESYSTEM.md`](core/FILESYSTEM.md)) | Done: tests and a shell |
| Checkpointing (snapshots): by hand or at fixed times, go back x checkpoints | `features/checkpoint/` | Done: tests and demos |
| Duplication (copy a page or a whole file without copying its data) | `features/duplication/` | Done: tests and demo |
| Recovery (commit, crash, mount) | `features/recovery/` | Done: tests and demo |
| Backup (send/receive) | `features/backup/` | To do |

## Build and run

```sh
make          # build everything into build/
make test     # core tests, then every feature's tests
make demo     # every feature's demo
make shell    # an interactive filesystem shell on fsh.img (tools/fsh.c)
make clean
```

This needs only a C11 compiler (clang on macOS, gcc on Linux) and `make`. The
build turns on strict warnings (`-Wconversion`, `-Wshadow`, …), and it currently
compiles with none.

## Layout

```text
btrfs/
├── Makefile                  finds every feature automatically
├── core/                     SHARED by all features: change with care
│   ├── include/              public headers: the only ones features may include
│   │   ├── disk.h            Disk       abstract simulated disk
│   │   ├── mem_disk.h        concrete Disk in RAM
│   │   ├── file_disk.h       concrete Disk in a file (survives the program)
│   │   ├── allocator.h       Allocator  reference count per block, commit marks
│   │   ├── cowtree.h         CowTree    copy-on-write tree: get, set, share, drop
│   │   ├── volume.h          Volume     live pages + root table of named trees
│   │   └── fs.h              Fs         files, directories, inodes, in the pages
│   ├── src/                  implementations (+ disk_impl.h, fs_impl.h: private)
│   │   ├── fs_block.c        bread/bwrite, superblock, alloc and free
│   │   ├── fs_inode.c        iget/iupdate, ialloc, ifree, itrunc, bmap
│   │   ├── fs_dir.c          directories, namei
│   │   ├── fs.c              mkfs and the file operations
│   │   └── fs_check.c        fsck
│   ├── tests/                test_core.c, test_fs.c
│   └── FILESYSTEM.md         the filesystem's data structures and flows
├── tools/
│   ├── fsh.c                 the filesystem shell
│   └── demo_session.txt      a sample session to pipe into it
└── features/
    ├── checkpoint/           checkpoints, timed checkpoints, go back x
    │   ├── include/checkpoint.h
    │   ├── src/checkpoint.c
    │   ├── tests/test_checkpoint.c
    │   ├── demo/demo_checkpoint.c
    │   ├── demo/demo_timed_checkpoints.c
    │   └── README.md
    ├── duplication/          same shape: include/ src/ tests/ demo/ README.md
    ├── recovery/             same shape
    └── backup/               README.md: where to start
```

```text
   checkpoint   duplication   recovery   backup     features (one owner each)
        \           |            |          /
         └───────── fs.h ──────────────────┘        core: the filesystem
                      │
                  volume.h                          core: pages + root table
                      │
                  cowtree.h
                      │
         allocator.h       disk.h ← mem_disk.h, file_disk.h
```

## Rules that keep features independent

1. **Core never includes a feature.** Dependencies point one way only: features →
   core.
2. **Features include only `core/include/*.h`**, and other features' `include/`
   if they really need them. The Makefile builds features *without* access to
   `core/src`, so a private header there can't be reached by accident.
3. **Each ADT is opaque.** A header declares `typedef struct X X;` and functions.
   The struct layout stays in its `.c` file.
4. **Every root you hold owns one reference.** That covers the live root and each
   root-table entry. Use `vol_root_add` / `vol_root_remove` /
   `vol_set_live_root`, which keep the counts right. After any change,
   `vol_check(v) == 0` must hold: it recounts every reference from scratch.
5. **Errors are negative errno values** (`-ENOENT`, `-EEXIST`, `-ENOSPC`, `-EINVAL`).
   0 means success.
6. **A change to core is a change for everyone.** Add to it rather than
   changing existing behaviour, and run `make test` (all features) before
   pushing.

## Adding a feature (for example, `backup`)

1. Create `features/backup/include/backup.h` (the public API) and
   `features/backup/src/backup.c`.
2. Add tests in `features/backup/tests/test_backup.c` and, optionally, a demo in
   `features/backup/demo/demo_backup.c`. Each file has its own `main()`.
3. Run `make test`. The Makefile picks the new folder up by itself.

## What core gives a feature

| Need | Core API |
|---|---|
| Files and directories | `fs_format`, `fs_open`, `fs_create`, `fs_mkdir`, `fs_write`, `fs_read`, `fs_unlink`, `fs_rmdir`, `fs_link`, `fs_truncate`, `fs_stat`, `fs_readdir` |
| The filesystem as it is in a checkpoint | `fs_open_tree` (read-only) |
| Which page holds part of a file | `fs_bmap` |
| Check the filesystem | `fs_check` (fsck) |
| Read and write live pages | `vol_write`, `vol_read`, `vol_erase`, `vol_write_page`, `vol_read_page` |
| Keep a named, read-only tree (snapshot, backup base) | `vol_root_add`, `vol_root_find`, `vol_root_remove`, `vol_root_each` |
| Find the n-th newest named tree | `vol_root_nth` |
| Point a page at an existing data block | `vol_map_page` |
| Save and load the volume's in-memory state | `vol_save`, `vol_load` |
| Protect the committed state from reuse | `vol_allocator` → `alloc_commit` |
| A disk that outlives the program | `file_disk_open` |
| Replace the live tree (rollback, restore) | `vol_set_live_root` |
| Read a page from any tree | `vol_read_tree` |
| Walk or change trees and blocks directly | `vol_tree` → `cowtree_get`, `cowtree_set`, `cowtree_share`, `cowtree_drop` |
| Reference counts, free space | `vol_allocator` → `alloc_refcount`, `alloc_ref`, `alloc_unref`, … |
| Raw block I/O | `vol_disk` → `disk_read`, `disk_write` |
| Verify you didn't break anything | `vol_check`, `vol_print` |

## Simplifications compared with real btrfs

- A fixed two-level page map (32 × 32 = 1,024 pages of 128 bytes) instead of a
  B+tree.
- The filesystem is a classic inode filesystem (like ext2 or xv6) stored in those
  pages, with Bach's allocation algorithms: 64 inodes, 12 direct blocks per inode
  (files up to 1,536 bytes), 46 names per directory. Btrfs instead keeps inodes and directory entries as items in its FS
  tree. No rename, permissions or timestamps.
- The root table is a small array saved inside the superblock. In btrfs it is the
  on-disk root tree.
- Reference counts are not stored on disk: mount rebuilds them by walking the
  trees. Btrfs keeps them in the extent tree.
- Blocks 1 to 16 of every disk are set aside for the two superblock slots.
- Timed checkpoints get the time from the caller, so there is no background thread.
