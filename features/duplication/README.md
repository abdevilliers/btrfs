# Feature: duplication (reflink copies)

`dup_copy_file(fs, src, dst)` is `cp --reflink=always`. The copy gets its own inode and
its own `EXTENT_DATA` items, but they point at the **same data extents**. Each extent's
reference count goes up, and it gains an `EXTENT_DATA_REF` back-reference for the new
inode. No data is read or written.

```sh
make test    # build/bin/test_duplication
make demo    # build/bin/demo_duplication: copy a file, show each extent's refs and back-references
```

## Files

| File | Contents |
|---|---|
| `include/duplication.h` | `dup_copy_file`, `dup_copy_range` (FICLONERANGE), `dup_is_shared` |
| `src/duplication.c` | A thin layer on the core's `fs_clone_range` (`btrfs_clone`) and `fs_fiemap` |
| `tests/test_duplication.c` | Behaviour checks, plus 3,000 random writes and copies against a model, with `btrfs_check` |
| `demo/demo_duplication.c` | The demo |

## What happens

```text
after cp --reflink /report.txt /copy.txt:
  (257 EXTENT_DATA 0)  → extent 7340032        extent tree:
  (258 EXTENT_DATA 0)  → extent 7340032          (7340032 EXTENT_ITEM 20480) refs 2
                                                 EXTENT_DATA_REF root 5 inode 257 count 1
                                                 EXTENT_DATA_REF root 5 inode 258 count 1

after writing 3 bytes at offset 5000 of the copy:
  (258 EXTENT_DATA 0)     → old extent, sectors 0                 refs 3 (inode 258 count 2)
  (258 EXTENT_DATA 4096)  → a NEW extent: the rewritten sector
  (258 EXTENT_DATA 8192)  → old extent, from sector 2 on
```

- The written sector goes to a new extent (copy-on-write). The copy's item is split
  around it, and both halves still reference the old extent.
- An extent is freed only when its last reference goes. Deleting the original leaves
  the copy whole.
- A copy can go to another subvolume.
- Files of 2048 bytes or less are inline: their data is in the tree, not in an extent.
  They are copied, as btrfs does.
