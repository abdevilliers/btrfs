/* duplication.h - copy without copying the data
 * (btrfs counterpart: reflink copies, `cp --reflink`).
 *
 * The destination page points at the SAME data block as the source and
 * the block's reference count goes up. Writing either copy later gives
 * it its own block (copy-on-write), so the two stay independent.
 */
#ifndef DUPLICATION_H
#define DUPLICATION_H

#include "volume.h"

int dup_copy_page(Volume *v, uint32_t from, uint32_t to);
int dup_copy_file(Volume *v, const char *src, const char *dst); /* 0 / -1 */

#endif
