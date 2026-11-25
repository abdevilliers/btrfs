/*
 * fault_disk.h - a Disk that wraps another one and can lose power.
 *
 * fault_disk_cut_after(d, n) lets n more writes reach the inner disk and
 * silently drops every write after them, as if the power went out in the
 * middle of whatever was being written. Reads always go to the inner disk,
 * so after btrfs_crash() the inner disk holds exactly what "survived".
 */
#ifndef FAULT_DISK_H
#define FAULT_DISK_H

#include <stdint.h>

#include "disk.h"

/* The inner disk stays owned by the caller; closing the wrapper does not
 * close it. */
Disk *fault_disk_wrap(Disk *inner);

void fault_disk_cut_after(Disk *d, uint64_t writes); /* drop writes after n more */
void fault_disk_restore(Disk *d);                    /* power is back            */
uint64_t fault_disk_dropped(const Disk *d);          /* writes lost so far       */

#endif
