# Feature: recovery

After a crash, the volume comes back exactly as it was at the last commit: the live
pages and every checkpoint. It is never left half-changed.

```sh
make test    # runs build/bin/test_recovery (with the other tests)
make demo    # runs build/bin/demo_recovery: commit, crash, mount, on a file-backed disk
```

## Files

| File | Contents |
|---|---|
| `include/recovery.h` | Public API: `rec_commit`, `rec_mount`, `rec_generation` |
| `src/recovery.c` | The superblock, commit and mount, using only `volume.h` |
| `tests/test_recovery.c` | Commit and crash, checkpoints surviving, a half-written commit, a file-backed disk, and 20,000 random operations with about 2,000 commits and 2,000 crashes compared with a model |
| `demo/demo_recovery.c` | The same story, printed step by step |

## Using it

```c
Disk *disk = file_disk_open("volume.img", 128);
Volume *v;
if (rec_mount(disk, &v) == -ENOENT)   /* no commit on this disk yet */
    v = vol_create(disk);

vol_write(v, 0, "hello");
rec_commit(v);                        /* durable from here */
```

## How it works

Three rules, two of them in core:

1. **Nothing the last commit uses is overwritten.** `alloc_commit` (core) marks the
   blocks in use at a commit; the allocator does not hand them out again until the
   next commit, even if their count drops to 0. `node_cow` (core) copies a node
   the last commit uses instead of changing it in place.
2. **A commit is one small write.** All pages and tree nodes are already on the
   disk, in blocks the last commit does not use. `rec_commit` writes a
   **superblock** that says where the trees are: the live root and the root table
   (from `vol_save`), a commit number and a checksum.
3. **Two superblock slots.** Commit number g goes into slot g % 2, so a commit
   never writes over the previous one. `rec_mount` reads both slots and uses the
   newest one whose checksum is correct. If a commit was cut off halfway, its slot
   fails the checksum and the previous commit is used.

```text
blocks:  | 1 ......... 8 | 9 ........ 16 | 17 ...............................
         |    slot 0     |    slot 1     |  pages and tree nodes (copy-on-write)
```

`rec_commit(v)`:

1. Fill in a superblock: commit number + 1, the state from `vol_save`, checksum.
2. `disk_flush`: the new pages and nodes are really on the disk.
3. Write the superblock into the slot the last commit did not use; `disk_flush`.
4. `alloc_commit`: protect this state's blocks; release those only the previous
   commit needed.

`rec_mount(disk, &v)`:

1. Read both slots; keep those with a correct checksum; take the higher commit
   number.
2. `vol_load` rebuilds the Volume from the saved state. Reference counts are not
   stored: they are recomputed by walking the live tree and every checkpoint.
3. `alloc_commit`: the loaded state is the committed one.

## What a crash means in the tests

`vol_destroy(v)` without a commit throws away everything in memory, which is what a
power cut does. Only the blocks on the disk remain, and `rec_mount` must rebuild
the volume from them.
