/*
 * volume.h - Volume: abstract data type for the live data and the table
 * of named trees kept alongside it.
 *
 * The live volume is a set of numbered pages (0 .. VOL_PAGES-1), each
 * holding a short text. Next to it, the volume keeps a ROOT TABLE: named,
 * read-only trees that share blocks with the live one (like btrfs's root
 * tree). Features build on the root table: a checkpoint is one entry, a
 * backup base could be another. Core does not know which feature added
 * an entry.
 */
#ifndef VOLUME_H
#define VOLUME_H

#include <stddef.h>
#include <stdint.h>

#include "allocator.h"
#include "cowtree.h"
#include "disk.h"

#define VOL_PAGES 16      /* pages are numbered 0 .. VOL_PAGES-1 */
#define VOL_NAME_MAX 31   /* longest name in the root table      */
#define VOL_MAX_ROOTS 8   /* entries the root table can hold     */

/* Blocks 1 .. VOL_META_BLOCKS are never used for pages or tree nodes.
 * They are set aside for metadata at a fixed place on the disk: the
 * recovery feature keeps its superblock there. */
#define VOL_META_BLOCKS 16
#define VOL_STATE_MAX 400 /* a buffer this big always fits vol_save() */

typedef struct Volume Volume;

/* ------------------------------------------------------------ pages */

Volume *vol_create(Disk *disk); /* a new, empty volume; the caller owns the disk */
void vol_destroy(Volume *v);    /* forgets the volume; the disk is not touched   */

/* Write a page. Data is never overwritten: the text goes to a new block. */
int vol_write(Volume *v, uint32_t page, const char *text);
int vol_erase(Volume *v, uint32_t page);
int vol_read(Volume *v, uint32_t page, char *buf, size_t len); /* -ENOENT if empty */

uint32_t vol_used_blocks(const Volume *v);

/* Recount every reference from scratch; returns how many blocks have a
 * wrong count (0 = consistent). */
int vol_check(Volume *v);

/* Print the live tree and every named tree with reference counts. */
void vol_print(Volume *v);

/* ------------------------------------- for features: trees and roots
 *
 * A tree is named by its root block. Every root held here (the live root
 * and each root-table entry) owns one reference to that block. */

/* The live tree's root. */
uint32_t vol_live_root(const Volume *v);

/* Make `root` the live tree: takes a reference to it, then drops the old
 * live tree (freeing whatever nothing else uses). */
void vol_set_live_root(Volume *v, uint32_t root);

/* Root table. vol_root_add takes a reference to `root`; vol_root_remove
 * drops it. Errors: -EINVAL (bad name), -EEXIST, -ENOSPC, -ENOENT. */
int vol_root_add(Volume *v, const char *name, uint32_t root);
int vol_root_find(Volume *v, const char *name, uint32_t *root);
int vol_root_remove(Volume *v, const char *name);

/* Entries in order of creation, newest first: n = 1 is the newest entry,
 * n = 2 the one added before it, and so on. `name` must hold
 * VOL_NAME_MAX + 1 bytes. -ENOENT if there are fewer than n entries. */
int vol_root_nth(Volume *v, uint32_t n, char *name, uint32_t *root);

typedef void (*vol_root_fn)(void *arg, const char *name, uint32_t root);
void vol_root_each(Volume *v, vol_root_fn fn, void *arg);

/* Read a page from any tree (the live one or a root-table entry). */
int vol_read_tree(Volume *v, uint32_t root, uint32_t page, char *buf, size_t len);

/* Point a live page at an existing data block (0 = erase). The live tree
 * takes over the caller's reference to `data`. -ENOSPC, -EINVAL. */
int vol_map_page(Volume *v, uint32_t page, uint32_t data);

/* Saving and loading (used by recovery). vol_save copies everything the
 * volume keeps in memory (the live root and the root table) into `buf`
 * and returns its size, or -ENOSPC if `len` is too small. vol_load builds
 * a Volume from such a buffer and the blocks already on `disk`; reference
 * counts are rebuilt by walking the trees. NULL if the buffer is bad. */
int vol_save(const Volume *v, void *buf, size_t len);
Volume *vol_load(Disk *disk, const void *buf, size_t len);

/* The building blocks, for features that work on blocks directly
 * (duplication, for example). */
CowTree *vol_tree(Volume *v);
Allocator *vol_allocator(Volume *v);
Disk *vol_disk(Volume *v);

#endif
