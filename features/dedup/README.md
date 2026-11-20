# Feature: duplication and deduplication (to do)

**Owner:** _[name]_

Two sides of one mechanism: several pages pointing at the **same** data block,
tracked by reference counts.

- **Duplication (reflink copy):** copy page A to page B without copying data.
  Point B at A's data block and give that block one more reference. A later write
  to either page gets a new block through copy-on-write, so the other page is
  unaffected.
- **Deduplication:** find pages whose data blocks hold identical bytes, point them
  all at one block, and free the duplicates.

## Suggested files

```text
include/dedup.h          int dup_copy_page(Volume *v, uint32_t from, uint32_t to);
                         int dedup_run(Volume *v, uint32_t *blocks_freed);
src/dedup.c
tests/test_dedup.c
demo/demo_dedup.c        (optional)
```

The Makefile picks these up automatically.

## Core API you will use

- `cowtree_get(vol_tree(v), vol_live_root(v), page)` gets a page's data block.
- `disk_read(vol_disk(v), blk, buf)` compares contents (hash first, then compare
  bytes before sharing).
- `alloc_ref(vol_allocator(v), blk)` adds a reference before handing a block to
  `cowtree_set`.
- `cowtree_set(vol_tree(v), &root, page, blk)` points a page at a block. It takes
  over your reference and drops the old block's.
- The live root changes when you call `cowtree_set` on it. Write the new root back
  with `vol_set_live_root`, or work out how to update it.
- Finish every test with `CHECK(vol_check(v) == 0)`.

**Watch out:** blocks shared with a checkpoint already have refcount > 1. Deduping
them is fine, because the counts take care of it, but test it together with
`features/checkpoint`.
