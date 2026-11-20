/*
 * checkpoint.h - checkpoints (snapshots) of a Volume.
 *
 * A checkpoint is a read-only copy of the live volume as it was when it
 * was taken. Taking one copies NOTHING: the checkpoint is an entry in the
 * volume's root table that points at the live tree's root, and that root
 * gains one reference. Copy-on-write in the core tree (cowtree.c) then
 * keeps the live volume and the checkpoint apart as pages change.
 *
 * Checkpoints can be taken by hand (ckpt_create) or automatically at
 * fixed times (CkptTimer). Rollback works by name, or by position: "go
 * back x checkpoints".
 *
 * Uses only the public core API (volume.h).
 */
#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#include <stddef.h>
#include <stdint.h>

#include "volume.h"

#define CKPT_NAME_MAX VOL_NAME_MAX

/* ------------------------------------------------- by name */

int ckpt_create(Volume *v, const char *name);   /* -EEXIST, -ENOSPC, -EINVAL */
int ckpt_read(Volume *v, const char *name, uint32_t page, char *buf, size_t len);
int ckpt_rollback(Volume *v, const char *name); /* live volume = checkpoint   */
int ckpt_delete(Volume *v, const char *name);   /* frees what only it used    */

/* ------------------------------------------------- by position
 *
 * Checkpoints are counted from the newest: x = 1 is the most recent
 * checkpoint, x = 2 the one taken before it, and so on.
 *
 * ckpt_rollback_back makes the live volume equal to the x-th newest
 * checkpoint. No checkpoint is deleted, so it is possible to go back 5
 * and later return to a newer one. -ENOENT if there are fewer than x
 * checkpoints (or x is 0). */
int ckpt_rollback_back(Volume *v, uint32_t x);

/* Name of the x-th newest checkpoint; `name` must hold CKPT_NAME_MAX + 1
 * bytes. -ENOENT if there are fewer than x. */
int ckpt_name_back(Volume *v, uint32_t x, char *name);

uint32_t ckpt_count(Volume *v); /* how many checkpoints exist */

/* ------------------------------------------------- at fixed times
 *
 * A CkptTimer takes a checkpoint every `interval` time units, at the
 * fixed times interval, 2 * interval, 3 * interval, ... The checkpoint
 * taken at time T is named "auto-T". Only the newest `keep` automatic
 * checkpoints are kept; older ones are deleted. Checkpoints made by hand
 * are never deleted by the timer.
 *
 * The timer does not read a clock itself. The caller passes the current
 * time to ckpt_timer_tick, for example time(NULL) for real seconds, or a
 * counter in a test. Call it as often as convenient: it only acts when a
 * checkpoint is due. */
typedef struct CkptTimer CkptTimer;

/* NULL if interval or keep is 0, or out of memory. `now` = current time. */
CkptTimer *ckpt_timer_create(Volume *v, uint64_t interval, uint32_t keep, uint64_t now);
void ckpt_timer_destroy(CkptTimer *t);

/* 1 = a checkpoint was taken, 0 = none due yet, negative = error. */
int ckpt_timer_tick(CkptTimer *t, uint64_t now);

uint64_t ckpt_timer_next(const CkptTimer *t); /* when the next one is due */

#endif
