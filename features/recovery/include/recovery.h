/*
 * recovery.h - make a Volume survive a crash.
 *
 * rec_commit(v) makes the volume's current state durable. If the program
 * stops for any reason afterwards (a crash, a power cut, or simply
 * exiting), rec_mount(disk, &v) brings back exactly the state of the last
 * commit: the live pages and every checkpoint. Changes made after the
 * last commit are lost, but the volume is never left half-changed.
 *
 * How it works:
 *   - Nothing the last commit uses is ever overwritten (copy-on-write in
 *     core, see alloc_commit and node_cow).
 *   - A commit writes ONE small record, the superblock, which says where
 *     the trees are. There are two superblock slots at a fixed place on
 *     the disk; each commit writes the slot the previous commit did not
 *     use. So if a commit is interrupted, the previous one is still whole.
 *   - Mount reads both slots and uses the newest one whose checksum is
 *     correct.
 *
 * Uses only the public core API (volume.h).
 */
#ifndef RECOVERY_H
#define RECOVERY_H

#include <stdint.h>

#include "volume.h"

/* Make the current state durable. 0 on success. */
int rec_commit(Volume *v);

/* Open the volume stored on `disk` as of its last commit. On success *out
 * is a Volume to use (and later vol_destroy). -ENOENT if the disk has no
 * commit: start a new volume with vol_create instead. */
int rec_mount(Disk *disk, Volume **out);

/* Number of the last commit on the disk (1, 2, 3, ...); 0 if none. */
uint32_t rec_generation(Disk *disk);

#endif
