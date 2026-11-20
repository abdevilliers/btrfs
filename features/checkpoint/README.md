# Feature: checkpointing (snapshots)

A checkpoint is a read-only copy of the live volume as it was when it was taken.
Taking one copies nothing, and it stays cheap: it costs only the blocks that change
after it.

```sh
make test    # runs build/bin/test_checkpoint (with the core tests)
make demo    # runs build/bin/demo_checkpoint: a step-by-step story with every refcount
             # and build/bin/demo_timed_checkpoints: timed checkpoints, then go back 3

build/bin/demo_timed_checkpoints 4    # go back 4 checkpoints instead (any x)
```

## Files

| File | Contents |
|---|---|
| `include/checkpoint.h` | Public API. By name: `ckpt_create`, `ckpt_read`, `ckpt_rollback`, `ckpt_delete`. By position: `ckpt_rollback_back`, `ckpt_name_back`, `ckpt_count`. At fixed times: `CkptTimer` |
| `src/checkpoint.c` | The implementation, using only `volume.h` |
| `tests/test_checkpoint.c` | Behaviour checks, plus 20,000 random operations compared with a model and `vol_check()` after every step |
| `demo/demo_checkpoint.c` | Prints both trees after each step |
| `demo/demo_timed_checkpoints.c` | Checkpoints every 10 time units, then goes back x (x from the command line) |

## How it works

| Operation | Implementation | Effect |
|---|---|---|
| `ckpt_create(v, "v1")` | `vol_root_add(v, "v1", vol_live_root(v))` | Root-table entry → live root; root refs +1 |
| `ckpt_read(v, "v1", p, …)` | `vol_root_find` + `vol_read_tree` | Reads the checkpoint's tree |
| `ckpt_rollback(v, "v1")` | `vol_set_live_root(v, root of v1)` | Live volume takes v1's tree; its old tree is dropped |
| `ckpt_delete(v, "v1")` | `vol_root_remove(v, "v1")` | Drops the entry's reference; frees what only v1 used |

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
==== 2. Take checkpoint "v1" ====
blocks written: 0   blocks in use: 7 (+0)   reference counts: OK

==== 3. Change page 1 in the live volume ====
live volume:
  root #25  refs 1
    leaf #26  refs 1   (pages 0-3)
      page 1  -> data #24  refs 1   "chapter 1, FINAL"
    leaf #22  refs 2  shared   (pages 4-7)
"v1":
  root #17  refs 1
    leaf #19  refs 1   (pages 0-3)
      page 1  -> data #20  refs 1   "chapter 1, draft"
    leaf #22  refs 2  shared   (pages 4-7)
blocks written: 3   blocks in use: 10 (+3)   reference counts: OK
```
