# btrfs: a btrfs-style copy-on-write storage project

This is a small, readable model of how btrfs stores data, built in C as abstract
data types. A shared **core** provides the copy-on-write tree and reference counts.
Each team member builds one **feature** on top of it, in its own folder.

| Feature | Folder | Status |
|---|---|---|
| Checkpointing (snapshots): by hand or at fixed times, go back x checkpoints | `features/checkpoint/` | Done: tests and demos |
| Duplication (copy a page without copying its data) | `features/duplication/` | Done: tests and demo |
| Recovery (commit, crash, mount) | `features/recovery/` | Done: tests and demo |
| Backup (send/receive) | `features/backup/` | To do |

## Build and run

```sh
make          # build everything into build/
make test     # core tests, then every feature's tests
make demo     # every feature's demo
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
│   │   └── volume.h          Volume     live pages + root table of named trees
│   ├── src/                  implementations (+ disk_impl.h, private)
│   └── tests/test_core.c
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
         └──────── volume.h ───────────────┘        core: public API
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
| Read and write live pages | `vol_write`, `vol_read`, `vol_erase` |
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

- A fixed two-level page map (16 pages) instead of a B+tree.
- Numbered pages instead of files and directories.
- The root table is a small array saved inside the superblock. In btrfs it is the
  on-disk root tree.
- Reference counts are not stored on disk: mount rebuilds them by walking the
  trees. Btrfs keeps them in the extent tree.
- Blocks 1 to 16 of every disk are set aside for the two superblock slots.
- Timed checkpoints get the time from the caller, so there is no background thread.
