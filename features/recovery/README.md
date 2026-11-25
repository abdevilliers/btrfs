# Feature: recovery

After a crash, the filesystem comes back exactly as it was at the last commit. That
includes every file, subvolume and checkpoint. It also comes back when a commit was cut
off, the primary superblock is damaged, or the newest root tree is.

```sh
make test    # build/bin/test_recovery
make demo    # build/bin/demo_recovery: a file-backed disk; crash; a cut-off commit; a damaged superblock
```

## Using it

```c
Disk *disk = file_disk_open("volume.img", 20000);
Btrfs *b;
if (rec_mount(disk, &b, NULL) == -ENOENT) {   /* no filesystem yet */
    btrfs_mkfs(disk, NULL);
    rec_mount(disk, &b, NULL);
}
Fs *fs = fs_open(b);
fs_write(fs, "/a", 0, "hello", 5);
rec_commit(b);                                /* durable from here */
```

## Why a crash cannot break it (all in core)

1. **Copy-on-write:** nothing the last commit uses is ever overwritten. New tree blocks
   and data go to free space.
2. **Pinned extents:** space freed in a transaction is not reused until the transaction
   has committed.
3. **The commit order** (`core/src/transaction.c`): write every new block, flush, then
   write the superblock pointing at the new root tree, to all its copies. Until the
   primary superblock is written, the disk still says "the previous commit".
   `core/tests/test_crash.c` cuts the power after every single write of a commit: the
   result is always the old state or the new one, never a mix.
4. **Verification:** every tree block read is checked against its checksum, its own
   address, and the generation its parent expects. Damage is found, not trusted.

## What `rec_mount` tries

| Step | btrfs equivalent | When it helps |
|---|---|---|
| 1. mount | `mount` | The normal case |
| 2. mount with backup roots | `mount -o usebackuproot` | The newest root tree is damaged: use the newest of the four backup roots that reads cleanly (usually the previous commit) |
| 3. super-recover, then 1 and 2 | `btrfs rescue super-recover` | The primary superblock (at 64 KiB) is damaged: copy the newest good mirror (at 64 MiB) over it. It needs a disk larger than 64 MiB |

`*how` says which step worked. `rec_generation(disk)` is the transaction id of the last
commit on the disk.

## What a crash means in the tests

`btrfs_crash(b)` throws away everything in memory without committing, as a power cut
does. `fault_disk_wrap` + `fault_disk_cut_after(d, n)` lets only n more writes reach the
disk, so the power fails part-way through a commit.
