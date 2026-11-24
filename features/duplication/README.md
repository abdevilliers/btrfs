# Feature: duplication

Copy a file, or a single page, without copying its data. `dup_copy_file(fs, src, dst)`
gives `dst` its own inode and pages, each pointing at the same data block as the
matching page of `src`. After `dup_copy_page(v, A, B)`, page B points
at the same data block as page A and the block's reference count goes up by one.
This is what btrfs calls a reflink copy (`cp --reflink`).

```sh
make test    # runs build/bin/test_duplication (with the other tests)
make demo    # runs build/bin/demo_duplication: cp a file, show every block and refcount
```

## Files

| File | Contents |
|---|---|
| `include/duplication.h` | Public API: `dup_copy_file`, `dup_copy_page`, `dup_is_shared` |
| `src/duplication.c` | The implementation, using only `volume.h` |
| `tests/test_duplication.c` | Behaviour checks, plus 5,000 random operations compared with a model and `vol_check()` after every step |
| `demo/demo_duplication.c` | Write a file, copy it, update the copy, delete the original; each block's disk block and refcount |

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

## Copying a whole file: `dup_copy_file(fs, src, dst)`

1. `fs_stat(src)`: it must be a file.
2. `fs_create(dst)`: a new inode from the free list.
3. For each page of `src`: `fs_bmap(src, i)` finds its volume page; `fs_bmap(dst, i,
   alloc)` gives `dst` a page of its own (empty, so it costs no disk block);
   `dup_copy_page` points it at the source page's data block (count + 1).
4. `fs_truncate(dst, size)`: the same size as `src`.

Writing either file later splits only the page written. In the shell: `cp FILE NEWNAME`.

## Demo output (abridged)

```text
==== 2. cp /report.txt /copy.txt  (dup_copy_file) ====
  /report.txt: inode 2, 500 bytes
    file block 0 -> fs block 34   -> disk block #19   refs 2  shared
    file block 1 -> fs block 35   -> disk block #133  refs 2  shared
    ...
  /copy.txt: inode 3, 500 bytes
    file block 0 -> fs block 38   -> disk block #19   refs 2  shared
    file block 1 -> fs block 39   -> disk block #133  refs 2  shared
    ...
==== 3. Update the copy: write "xyz" at byte 130 (file block 1) ====
  /copy.txt: ...
    file block 1 -> fs block 39   -> disk block #17   refs 1
```

The copy has its own inode (`ialloc`) and its own filesystem blocks (`alloc`), but
they point at the original's disk blocks. Updating the copy splits only the block
written.
