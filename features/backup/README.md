# Feature: backup (send / receive) (to do)

**Owner:** _[name]_

This is modelled on `btrfs send` / `btrfs receive`. It turns the difference between two
read-only snapshots into a stream of changes, and applies that stream to another
filesystem.

- **Full backup:** every file of a snapshot.
- **Incremental backup:** only what differs between a parent snapshot and a newer one.
  Sharing makes this cheap: btrfs's `btrfs_compare_trees` walks both FS trees together.
  When two node pointers name the **same block** (same bytenr and generation), nothing
  below them changed, so it skips that subtree.

## Suggested files

```text
include/backup.h    int backup_send(Btrfs *b, uint64_t parent_snap, uint64_t snap, FILE *out);
                    int backup_receive(Fs *dst, const char *path, FILE *in);
src/backup.c
tests/test_backup.c
demo/demo_backup.c  (optional)
```

## Core API you will use

- Snapshots to send: `ckpt_create` (features/checkpoint) or `fs_subvol_snapshot(..., 1, &id)`.
  Find the parent of an incremental send with `fs_subvol_info`: the `parent_uuid` and
  `uuid` fields are what btrfs uses.
- Reading a snapshot's items: `btrfs_tree_search(b, snap_id, &min, &max, fn, arg)` gives
  every `INODE_ITEM`, `INODE_REF`, `DIR_INDEX` and `EXTENT_DATA` in key order. Compare two
  snapshots key by key.
- File contents: `fs_open_subvol(b, snap_id)` then `fs_read`.
- On receive, use `fs_subvol_create`, then the normal file operations. Finish with
  `fs_subvol_snapshot(..., 1, ...)` so the received copy is read-only, as btrfs receive
  leaves it.

To skip shared subtrees without visiting every item, you need to see node pointers.
Add a small core function, such as `btrfs_tree_walk(b, tree_id, fn)` reporting each
block's bytenr and generation, to `fs.h`. Then run `make test` for all features.
