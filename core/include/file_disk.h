/*
 * file_disk.h - a concrete Disk whose blocks live in a file, so the data
 * is still there after the program ends (needed by recovery).
 */
#ifndef FILE_DISK_H
#define FILE_DISK_H

#include "disk.h"

/* Open the disk image at `path`, creating it if it does not exist.
 * NULL if the file cannot be opened. */
Disk *file_disk_open(const char *path, uint32_t nblocks);

#endif
