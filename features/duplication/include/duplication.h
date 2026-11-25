/*
 * duplication.h - copy a file without copying its data: a reflink copy
 * (cp --reflink=always, the FICLONE / FICLONERANGE ioctls).
 *
 * The copy gets its own inode and its own EXTENT_DATA items, but they point
 * at the same data extents as the original: each extent gains one
 * EXTENT_DATA_REF back-reference and its refs count goes up. Nothing is
 * read or written for the data itself.
 *
 * Afterwards the files are independent: writing either one puts the new
 * data in a new extent (copy-on-write), and only the item for the written
 * range changes. An extent is freed when its last reference goes.
 *
 * Uses only the public core API (fs.h).
 */
#ifndef DUPLICATION_H
#define DUPLICATION_H

#include <stdint.h>

#include "fs.h"

/* Copy a whole file. dst is created; -EEXIST if it exists, -EISDIR. The
 * two files may be in different subvolumes. */
int dup_copy_file(Fs *fs, const char *src, const char *dst);

/* Share [src_off, src_off + len) of src at dst_off of dst (both files
 * exist). Offsets and len in whole sectors (BLOCK_SIZE), except that the
 * range may end at the end of src. */
int dup_copy_range(Fs *fs, const char *src, uint64_t src_off, const char *dst, uint64_t dst_off,
                   uint64_t len);

/* 1 if byte off_a of file a and byte off_b of file b are the same byte on
 * the disk (shared), else 0. */
int dup_is_shared(Fs *fs, const char *a, uint64_t off_a, const char *b, uint64_t off_b);

#endif
