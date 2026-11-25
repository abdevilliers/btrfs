/*
 * checkpoint.h - checkpoints of the live filesystem, as btrfs read-only
 * snapshots (the way snapper uses them).
 *
 * Layout, in the top-level subvolume (5):
 *
 *   /@                 the live subvolume: the default one, what fs_open opens
 *   /.checkpoints/NAME a read-only snapshot of the live subvolume
 *
 * ckpt_setup makes this layout once, right after mkfs. A checkpoint copies
 * one tree block (the snapshot's root); everything else is shared until
 * copy-on-write separates it. Creating one commits the transaction, as
 * `btrfs subvolume snapshot` does, so it survives a crash at once.
 *
 * Rolling back does what `snapper rollback` does: a WRITABLE snapshot of
 * the checkpoint becomes the new live subvolume (the default), and the
 * old live subvolume is deleted. Open the live filesystem again with
 * fs_open() afterwards: handles on the old one return -ESTALE.
 *
 * Uses only the public core API (fs.h).
 */
#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#include <stdint.h>

#include "fs.h"

#define CKPT_NAME_MAX FS_NAME_MAX
#define CKPT_DIR "/.checkpoints"

/* Make /@ the live (default) subvolume and /.checkpoints. 0 if it is
 * already set up. */
int ckpt_setup(Btrfs *b);

/* ------------------------------------------------------- by name */

int ckpt_create(Btrfs *b, const char *name);   /* -EEXIST, -EINVAL */
int ckpt_rollback(Btrfs *b, const char *name); /* live = a copy of it   */
int ckpt_delete(Btrfs *b, const char *name);   /* frees what only it used, after the next commit */

/* The filesystem as it was in a checkpoint: read-only (changes return
 * -EROFS). NULL if there is no such checkpoint. */
Fs *ckpt_open_fs(Btrfs *b, const char *name);

/* ------------------------------------------------------- by position
 *
 * x = 1 is the newest checkpoint, x = 2 the one before it, and so on.
 * Going back does not delete any checkpoint, so it is possible to go
 * back 5 and later return to a newer one. -ENOENT if there are fewer
 * than x (or x is 0). */
int ckpt_rollback_back(Btrfs *b, uint32_t x);
int ckpt_name_back(Btrfs *b, uint32_t x, char *name); /* name: CKPT_NAME_MAX + 1 bytes */
uint32_t ckpt_count(Btrfs *b);

/* ------------------------------------------------------- at fixed times
 *
 * A CkptTimer takes a checkpoint every `interval` time units, at the
 * fixed times interval, 2 * interval, ..., named "auto-T". Only the newest
 * `keep` automatic checkpoints are kept; ones made by hand are never
 * deleted by the timer. The caller passes the time to ckpt_timer_tick
 * (time(NULL), or a counter in a test). */
typedef struct CkptTimer CkptTimer;

CkptTimer *ckpt_timer_create(Btrfs *b, uint64_t interval, uint32_t keep, uint64_t now);
void ckpt_timer_destroy(CkptTimer *t);
int ckpt_timer_tick(CkptTimer *t, uint64_t now); /* 1 = taken, 0 = not due, < 0 error */
uint64_t ckpt_timer_next(const CkptTimer *t);

#endif
