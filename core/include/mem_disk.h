/*
 * mem_disk.h - a concrete Disk whose blocks live in RAM.
 */
#ifndef MEM_DISK_H
#define MEM_DISK_H

#include "disk.h"

Disk *mem_disk_create(uint32_t nblocks); /* NULL if out of memory */

#endif
