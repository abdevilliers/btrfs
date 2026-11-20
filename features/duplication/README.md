# Feature: duplication

Copy a page without copying its data. After `dup_copy_page(v, A, B)`, page B points
at the same data block as page A and the block's reference count goes up by one.
This is what btrfs calls a reflink copy (`cp --reflink`).

```sh
make test    # runs build/bin/test_duplication (with the other tests)
make demo    # runs build/bin/demo_duplication: the tree and every refcount after each step
```

## Files

| File | Contents |
|---|---|
| `include/duplication.h` | Public API: `dup_copy_page`, `dup_is_shared` |
| `src/duplication.c` | The implementation, using only `volume.h` |
| `tests/test_duplication.c` | Behaviour checks, plus 5,000 random operations compared with a model and `vol_check()` after every step |
| `demo/demo_duplication.c` | Duplicate, change the duplicate, erase the original |

## How it works

No new data structure is needed: the reference count that core keeps for every
block is what makes sharing safe.

`dup_copy_page(v, from, to)` is three steps:

1. `blk = cowtree_get(...)`: find the data block of page `from`.
2. `alloc_ref(blk)`: one more page will point at it, so its count goes up by one.
3. `vol_map_page(v, to, blk)`: point page `to` at the block. Core drops whatever
   `to` held before, and copies the tree path first if a checkpoint shares it.

The data block itself is never read or written.

Afterwards the two pages are independent, with no extra code here:

| Later operation | What core does | Result |
|---|---|---|
| Write page B | New text goes to a new block; the shared block's count goes 2 → 1 | Page A keeps the old data |
| Erase page A | The shared block's count goes 2 → 1 | Page B keeps the data |
| Erase both | The count reaches 0 | The block is freed |

## Demo output (abridged)

```text
==== 2. Duplicate page 0 into page 1 and page 5 ====
live volume:
  root #17  refs 1
    leaf #19  refs 1   (pages 0-3)
      page 0  -> data #18  refs 3  shared   "quarterly report"
      page 1  -> data #18  refs 3  shared   "quarterly report"
    leaf #20  refs 1   (pages 4-7)
      page 5  -> data #18  refs 3  shared   "quarterly report"
```
