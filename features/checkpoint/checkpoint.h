/* checkpoint.h - named, read-only copies of the whole filesystem
 * (btrfs counterpart: snapshots).
 *
 * A checkpoint is one root-table entry pointing at a tree root.
 * Creating one copies 0 blocks: it just adds a reference to the live
 * root. Later writes copy only the path they change (node_cow).
 *
 * Checkpoints are numbered by age. "1 back" is the newest checkpoint,
 * "2 back" the one before it, and so on, so you can roll back to ANY
 * earlier checkpoint, not just the last one.
 */
#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#include "volume.h"

int ckpt_create(Volume *v, const char *name);          /* 0 ok, -1 error */
int ckpt_delete(Volume *v, const char *name);

/* Fill out[] with checkpoints, newest first (out[0] = "1 back"). */
int ckpt_list(Volume *v, RootEntry out[MAX_CKPT]);

/* A read-only tree for a checkpoint: pass it to fs_read / fs_list. */
int ckpt_read(Volume *v, const char *name, CowTree *out);

/* Make the live filesystem equal to a checkpoint. The checkpoints stay
 * (including newer ones), so you can also roll forward again.
 *   keep_newer = 0 deletes checkpoints newer than the target. */
int ckpt_rollback(Volume *v, const char *name, int keep_newer);
int ckpt_rollback_back(Volume *v, int n_back, int keep_newer);   /* n >= 1 */

#endif
