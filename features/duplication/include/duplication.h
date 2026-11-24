/*
 * duplication.h - copy a page without copying its data.
 *
 * After dup_copy_page(v, A, B), page B points at the SAME data block as
 * page A, and that block's reference count goes up by one. Nothing is
 * read or written for the data itself (this is what btrfs calls a
 * reflink copy, `cp --reflink`).
 *
 * The two pages stay independent: writing either one later puts the new
 * text in a new block (copy-on-write), and the other page keeps the old
 * block.
 *
 * Uses only the public core API (volume.h).
 */
#ifndef DUPLICATION_H
#define DUPLICATION_H

#include <stdint.h>

#include "fs.h"
#include "volume.h"

/* Make page `to` a duplicate of page `from` (replacing what `to` held).
 * -ENOENT if `from` is empty, -EINVAL for a bad page number, -ENOSPC. */
int dup_copy_page(Volume *v, uint32_t from, uint32_t to);

/* 1 if both pages hold data and share one data block, else 0. */
int dup_is_shared(Volume *v, uint32_t a, uint32_t b);

/* Copy a whole file without copying its data (cp --reflink).
 * `dst` gets a new inode of its own, but each of its pages points at the
 * same data block as the matching page of `src`. Writing either file
 * later splits only the page written. -EEXIST if dst exists, -EISDIR. */
int dup_copy_file(Fs *fs, const char *src, const char *dst);

#endif
