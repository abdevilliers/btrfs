# Feature: checkpointing (snapshots)

A checkpoint is a read-only copy of the live volume as it was when it was taken.
Taking one copies nothing, and it stays cheap: it costs only the blocks that change
after it.

```sh
make test    # runs build/bin/test_checkpoint (with the core tests)
make demo    # runs build/bin/demo_checkpoint: files created, checkpointed, updated,
             # read from the checkpoint, rolled back
             # and build/bin/demo_timed_checkpoints: timed checkpoints, then go back 3

build/bin/demo_timed_checkpoints 4    # go back 4 checkpoints instead (any x)
```

## Files

| File | Contents |
|---|---|
| `include/checkpoint.h` | Public API. `ckpt_open_fs`: the filesystem as it was in a checkpoint, read-only. By name: `ckpt_create`, `ckpt_read`, `ckpt_rollback`, `ckpt_delete`. By position: `ckpt_rollback_back`, `ckpt_name_back`, `ckpt_count`. At fixed times: `CkptTimer` |
| `src/checkpoint.c` | The implementation, using only `volume.h` |
| `tests/test_checkpoint.c` | Behaviour checks, plus 20,000 random operations compared with a model and `vol_check()` after every step |
| `demo/demo_checkpoint.c` | Files made with the file operations; checkpoint (0 blocks written), update, read the old version, roll back |
| `demo/demo_timed_checkpoints.c` | A file updated over time, checkpoints every 10 time units, then go back x (x from the command line) |

## How it works

| Operation | Implementation | Effect |
|---|---|---|
| `ckpt_create(v, "v1")` | `vol_root_add(v, "v1", vol_live_root(v))` | Root-table entry → live root; root refs +1 |
| `ckpt_read(v, "v1", p, …)` | `vol_root_find` + `vol_read_tree` | Reads the checkpoint's tree |
| `ckpt_rollback(v, "v1")` | `vol_set_live_root(v, root of v1)` | Live volume takes v1's tree; its old tree is dropped |
| `ckpt_delete(v, "v1")` | `vol_root_remove(v, "v1")` | Drops the entry's reference; frees what only v1 used |

## Checkpoints of the filesystem

The filesystem (`core/FILESYSTEM.md`) lives in the volume's pages, so a checkpoint
is a snapshot of every file, directory and inode, and of both free lists. Nothing
here knows about files: sharing the root is enough.

```c
ckpt_create(v, "monday");
fs_write(fs, "/report.txt", 0, "FINAL", 5);        /* the live filesystem changes */

Fs *old = ckpt_open_fs(v, "monday");               /* monday's filesystem, read-only */
fs_read(old, "/report.txt", 0, buf, sizeof buf);   /* the old text */
fs_close(old);

ckpt_rollback(v, "monday");                        /* files, directories, inodes: all back */
```

## Go back x checkpoints

`ckpt_rollback_back(v, x)` makes the live volume equal to the x-th newest
checkpoint: x = 1 is the most recent one, x = 2 the one before it, and so on. The
caller chooses x. It returns `-ENOENT` if fewer than x checkpoints exist, and then
nothing changes.

The root table gives every entry a sequence number when it is added, so
`vol_root_nth(v, x, …)` finds the x-th newest entry, and the rest is an ordinary
rollback. No checkpoint is deleted by a rollback, so after going back 4 it is
still possible to return to a newer one.

```c
ckpt_rollback_back(v, 1);   /* the last checkpoint          */
ckpt_rollback_back(v, 3);   /* three checkpoints back       */
```

## Checkpoints at fixed times

A `CkptTimer` takes a checkpoint every `interval` time units, at the fixed times
interval, 2 × interval, 3 × interval, … The checkpoint for time T is named
`auto-T`. Only the newest `keep` automatic checkpoints are kept (the root table
holds 8 entries in total); checkpoints made by hand are never deleted by the timer.

The timer does not read a clock. The program passes the current time to
`ckpt_timer_tick`, so tests can use a counter and a real program can use seconds:

```c
CkptTimer *t = ckpt_timer_create(v, 60, 5, (uint64_t)time(NULL)); /* every 60 s, keep 5 */
for (;;) {
    /* ... the program's normal work: vol_write, vol_read ... */
    ckpt_timer_tick(t, (uint64_t)time(NULL));   /* takes one only when it is due */
}
```

If the program did not tick for a while and several fixed times passed, one
checkpoint is taken for the latest of them.

## How copy-on-write keeps them apart

The heavy lifting is the core copy-on-write rule, `node_cow()` in
`core/src/cowtree.c`. Before changing a node, if its refcount is 1 it is changed in
place. If its refcount is greater than 1 (a checkpoint shares it), it is copied
instead: each child gets one more reference, and the original loses one. (A third
case, for nodes the last commit uses, belongs to recovery.) That rule lives in
core because every feature relies on it.

## Demo output (abridged)

```text
==== 2. Take checkpoint "monday" ====
disk blocks written: 0   disk blocks in use: 123   free inodes: 59   fsck: clean   reference counts: OK

==== 3. Update report.txt, delete notes.txt, create todo.txt ====
  /docs/report.txt     inode 3   "v2: FINAL version"
  /docs/notes.txt      (no such file)
  /docs/todo.txt       inode 4   "buy milk"

==== 4. Look inside "monday" (read-only) ====
  /docs/report.txt     inode 3   "v1: first draft"
  /docs/notes.txt      inode 4   "meeting at 10"
  /docs/todo.txt       (no such file)

==== 5. Roll back to "monday" ====
  /docs/report.txt     inode 3   "v1: first draft"
  /docs/notes.txt      inode 4   "meeting at 10"
disk blocks written: 0   ...   fsck: clean   reference counts: OK
```

`ifree` put notes.txt's inode (4) back in the free inode list, and `ialloc` gave it to
todo.txt. The checkpoint still holds the old inode table, so rollback brings notes.txt
back as inode 4.
