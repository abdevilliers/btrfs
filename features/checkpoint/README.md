# Feature: checkpointing (snapshots)

A checkpoint is a **read-only btrfs snapshot** of the live subvolume. Taking one copies a
single tree block, the root of the subvolume's FS tree. Everything else is shared until
copy-on-write separates it.

```sh
make test    # build/bin/test_checkpoint
make demo    # build/bin/demo_checkpoint: files, checkpoint, change, read the old version, roll back
             # build/bin/demo_timed_checkpoints [x]: a checkpoint every 10 time units, go back x
```

## The layout (`ckpt_setup`, once after mkfs)

```text
top-level subvolume (5)
├── @                  the live subvolume: the default one, what fs_open() opens
└── .checkpoints/
    ├── monday         read-only snapshot of @
    └── tuesday        read-only snapshot of @
```

This is the "flat" layout distributions use with snapper. The checkpoints live outside
the live subvolume, so a checkpoint never contains the others.

## Files

| File | Contents |
|---|---|
| `include/checkpoint.h` | Public API. Setup: `ckpt_setup`. By name: `ckpt_create`, `ckpt_rollback`, `ckpt_delete`, `ckpt_open_fs`. By position: `ckpt_rollback_back`, `ckpt_name_back`, `ckpt_count`. At fixed times: `CkptTimer` |
| `src/checkpoint.c` | The implementation, using only `fs.h` |
| `tests/test_checkpoint.c` | Behaviour checks, plus 1,500 random operations against a model, with `btrfs_check` every 100 |
| `demo/` | The two demos |

## How it works

| Operation | btrfs equivalent | What the core does |
|---|---|---|
| `ckpt_create(b, "monday")` | `btrfs subvolume snapshot -r /@ /.checkpoints/monday` | The snapshot is made inside a commit: @'s root block is copied for the new subvolume, and each child gains a `TREE_BLOCK_REF` from it. Creating one **commits**, as btrfs does |
| `ckpt_open_fs(b, "monday")` | mount `-o subvol=.checkpoints/monday` | A handle on the snapshot. Changes return `-EROFS` |
| `ckpt_rollback(b, "monday")` | `snapper rollback` | A **writable** snapshot of monday becomes the default subvolume, and the old live subvolume is deleted. Then it commits. Open the live filesystem again with `fs_open`: a handle on the old one returns `-ESTALE` |
| `ckpt_delete(b, "monday")` | `btrfs subvolume delete` | Its name goes now. After the next commit, the cleaner drops one reference to each of its blocks and frees what nothing else uses |

Checkpoints are ordered by the transaction that created them (`otransid`), newest first.
That gives "go back x": `ckpt_rollback_back(b, 3)` is the third newest. No checkpoint is
deleted by a rollback, so you can still go forward again afterwards.

## Checkpoints at fixed times

A `CkptTimer` takes a checkpoint every `interval` time units, named `auto-T`, and keeps
the newest `keep` of them. Checkpoints made by hand are never deleted. The timer reads no
clock: pass `time(NULL)` to `ckpt_timer_tick`, or a counter in a test. If several fixed
times were missed, one checkpoint covers them.

## Where the sharing comes from

The heavy lifting is the core's `update_ref_for_cow` (`core/src/ctree.c`). When a block
shared with a snapshot is copied, the children's references are adjusted so that both
trees still count them. See `core/FILESYSTEM.md` §3 and §7.
