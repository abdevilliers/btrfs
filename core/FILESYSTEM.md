# The filesystem (`core/include/fs.h`, `core/src/fs*.c`)

A Unix-style filesystem (superblock, inode table, directories, files) stored inside the
copy-on-write volume. It uses the classic allocation algorithms from Maurice Bach, *The
Design of the UNIX Operating System*, chapter 4:

| Algorithm | Code | Job |
|---|---|---|
| `ialloc` | `fs_inode.c` | Assign a free inode to a new file or directory |
| `ifree` | `fs_inode.c` | Release an inode when its last link is removed |
| `alloc` | `fs_block.c` (`balloc`) | Assign a free data block to a file |
| `free` | `fs_block.c` (`bfree`) | Release a data block |
| `namei` | `fs_dir.c` | Turn a path into an inode number |
| `bmap` | `fs_inode.c` | Turn a byte offset in a file into a data block |

The files:

| File | Contents |
|---|---|
| `fs_impl.h` | On-disk structures (private) |
| `fs_block.c` | `bread` / `bwrite`, the superblock, `alloc` and `free` |
| `fs_inode.c` | `iget` / `iupdate`, `ialloc`, `ifree`, `itrunc`, `bmap` |
| `fs_dir.c` | Directory entries, `namei` |
| `fs.c` | `mkfs` and the file operations |
| `fs_check.c` | fsck |

Every structure lives in the volume's pages, so everything below the filesystem keeps
working without knowing about files:

- **Checkpointing** snapshots the whole filesystem, including the inode table and both
  free lists.
- **Recovery** brings it all back after a crash.
- **Duplication** can share a file's data blocks with a copy.

```sh
make test                                   # core/tests/test_fs.c: fsck after every step
make shell                                  # an interactive shell (tools/fsh.c)
build/bin/fsh demo.img < tools/demo_session.txt
```

In the shell, `sb` prints the superblock's two free lists, so you can watch `ialloc`,
`ifree`, `alloc` and `free` at work.

## 1. Layout

One filesystem block is one 128-byte volume page; there are 1,024 of them.

```text
block:  0            1 ............ 32     33 ............................ 1023
      +------------+--------------------+-----------------------------------+
      | superblock |    inode table     |            data blocks            |
      |            | 64 inodes, 2/block | file contents, directory entries, |
      |            |                    | and the free block list's blocks  |
      +------------+--------------------+-----------------------------------+
```

## 2. The data structures

### Superblock (block 0)

```c
typedef struct {
    uint32_t magic, ninodes, nblocks, data_start, root_ino;
    uint32_t tfree;              /* total free data blocks                       */
    uint32_t tinode;             /* total free inodes                            */
    uint32_t nfree;              /* entries used in free[]                       */
    uint32_t free[12];           /* free block numbers; free[0] = the block that
                                    holds the next part of the list (0 = end)    */
    uint32_t ninode;             /* entries used in inode[]                      */
    uint32_t inode[8];           /* free inode numbers                           */
    uint32_t rinode;             /* the remembered inode                         */
} Super;
```

As in Unix, each operation keeps an **in-core copy** of the superblock: it is loaded when
the operation starts, changed in memory by the algorithms (which set a "modified" flag),
and written back once when the operation ends.

### Inode (64 bytes)

```c
typedef struct {
    uint16_t type;           /* 0 = free, 1 = file, 2 = directory   */
    uint16_t nlink;          /* directory entries pointing here     */
    uint32_t size;           /* bytes                               */
    uint32_t pad[2];
    uint32_t direct[12];     /* data block of each file block       */
} Dinode;
```

- Inode `n` is in block `1 + n / 2`. Inode 0 is never used; inode 1 is `/`.
- **Type 0 means free.** That is how `ialloc`'s search recognises a free inode.
- 12 direct blocks × 128 bytes = 1,536 bytes, the largest file.

### Directory entry (32 bytes, 4 per block)

`{ uint32_t ino; char name[28]; }`, where inode 0 marks an empty slot. Every directory has
`.` and `..`.

## 3. `ialloc` and `ifree`: the free inode list

The superblock caches up to 8 free inode numbers, plus the **remembered inode**.

```text
superblock.inode[] = [ 8  7  6  5  4  3  2 ]     ninode = 7
                                           ^ ialloc takes this one next (2)
superblock.rinode  = 8
```

### `ialloc` (Bach, figure 4.12)

```text
while (not done) {
    if (super block inode list is empty) {
        search the inode table, from the remembered inode upwards,
            for inodes of type 0, until the list is full or the table ends;
        if (none found) return "no inode";              -> -ENOSPC
        remembered inode = the highest one found;
    }
    take an inode number from the super block list;
    read the inode (iget);
    if (it is not free after all) continue;
    initialise it (type, nlink = 0, size = 0, no blocks);
    write it to the inode table;
    total free inodes - 1;
    return the inode;
}
```

### `ifree` (Bach, figure 4.14)

```text
total free inodes + 1;
if (super block inode list is full) {
    if (inode number < remembered inode)
        remembered inode = inode number;
} else
    put the inode number in the super block list;
```

Before calling `ifree`, the caller (`iremove`, Bach's `iput`) frees the file's blocks and
sets the inode's type to 0.

**Why the remembered inode works:** every free inode that is not in the list is at or
above the remembered inode. So a search that starts there never misses one, and never
has to scan the whole table. `ifree` lowers the remembered inode exactly when it cannot
put an inode in the list. fsck checks this rule.

**Worked example** (this is `test_ialloc_ifree` in `core/tests/test_fs.c`):

| Step | List (top on the right) | Remembered | Result |
|---|---|---|---|
| mkfs: `ialloc` for `/` scans 1..8, takes 1 | 8 7 6 5 4 3 2 | 8 | `/` = inode 1 |
| Create 7 files | (empty) | 8 | Inodes 2..8 |
| Create `/f9`: the list is empty, so scan from 8 | 16 15 14 13 12 11 10 | 16 | Inode 9 |
| Delete inode 3: the list has room | 16 … 10 3 | 16 | |
| Delete inode 4: the list is full | 16 … 10 3 | **4** | |
| Create 8 files | (empty) | 4 | Inodes 3, 10..16 |
| Create one more: scan from 4 | 23 … 17 | 23 | **Inode 4** is found |

## 4. `alloc` and `free`: the chained free block list

The superblock holds up to 12 free block numbers. `free[0]` is special: it is the block
that holds the **next** part of the list (or 0 at the end). So the free blocks form a
chain of lists:

```text
superblock             block 40               block 52
nfree = 7              nfree = 12             nfree = 12
free[0] = 40 -------->  free[0] = 52 -------->  free[0] = 64 --> ... --> free[0] = 0 (end)
free[1..6] = 39..34     free[1..11] = 11       free[1..11] = 11
             ^ next     more free blocks       more free blocks
```

Every block in the chain is free, including the ones that hold a part of the list.

### `alloc` (Bach, figure 4.19)

```text
remove a block number from the super block list (the top entry);
if (it was free[0], the last entry) {
    read that block;
    copy the block numbers in it into the super block list;
}
zero the block;
total free blocks - 1;
mark the super block modified;
return the block;
```

### `free` (Bach, figure 4.20; Unix V7 `free`)

```text
if (super block list is full) {
    write the super block list into the block being freed;
    empty the super block list;           /* that block becomes free[0] */
}
put the block number in the super block list;
total free blocks + 1;
mark the super block modified;
```

**`mkfs`** builds the chain by calling `free` for every data block, from 1023 down to 33,
so the lowest numbers are handed out first. `/` then takes block 33 with `alloc`.

**Worked example** (`test_alloc_free`): right after mkfs the list is `[link 40] 39 38
37 36 35 34`.
- A one-block file gets block 34, the top entry.
- Allocating the next 6 blocks takes 35..39, and then 40, which is `free[0]`. Its list of
  12 is copied into the superblock, and block 40 itself goes to the file.
- Deleting that file frees its blocks. The first one freed finds the list full, so the list
  is written into it and it becomes the new `free[0]`.

## 5. How the file operations use them

| Operation | Steps |
|---|---|
| `fs_create` | `namei` the parent → **`ialloc`** → add `{ino, name}` to the parent directory (`alloc` a new directory block if all are full) → `nlink = 1` |
| `fs_mkdir` | Same, plus the entries `.` and `..`, `nlink = 2`, and the parent's `nlink` + 1 |
| `fs_write` | For each file block touched: `bmap` (**`alloc`** if missing) → read → copy the bytes in → write. Then `size`, and write the inode |
| `fs_read` | `namei` → for each block: `bmap` → read (a missing block reads as zeros) |
| `fs_truncate` | Shrink: `itrunc` (**`free`** the blocks past the end) and zero the tail of the last block. Grow: just set `size` |
| `fs_unlink` | Remove the entry → `nlink` − 1 → if 0: `itrunc` (**`free`** every block), type = 0, **`ifree`** |
| `fs_rmdir` | Only if empty: the same, and the parent's `nlink` − 1 |
| `fs_link` | Add a second entry for a file → `nlink` + 1 |

### `namei`

Bach's `namei` turns `/docs/report.txt` into an inode number:
1. Start at inode 1 (`/`).
2. Search its entries for `docs` → inode 2.
3. Search inode 2's entries for `report.txt` → inode 3.

## 6. Each operation is all or nothing

One operation may change several blocks: `create` touches an inode block, a directory
block and the superblock. If the disk fills up halfway, the filesystem would be left
inconsistent. Copy-on-write makes the fix cheap:

```text
hold the current tree (share its root: +1 reference)
load the superblock
do the operation
write the superblock back if it changed
if anything failed: make the held tree the live one again    -> undo everything
let the held tree go
```

## 7. How the advanced features sit on top

```text
     checkpoint            duplication            recovery
  (ckpt_create,          (dup_copy_file:        (rec_commit,
   ckpt_open_fs,          ialloc + alloc,        rec_mount,
   ckpt_rollback_back)    share data blocks)     then fs_open)
          \                     |                     /
           +------------- the filesystem ------------+
                 create, write, read, update, delete
                 ialloc/ifree, alloc/free, namei, bmap
                               |
                 the copy-on-write volume (pages)
```

- **Checkpoint:** shares the root of the tree holding every block above, so it is a
  snapshot of all files and of both free lists. `ckpt_open_fs` reads it as a filesystem.
- **Duplication:** `dup_copy_file` uses `fs_create` (`ialloc`) and `fs_bmap` (`alloc`)
  to make a new file, then points each new block at the original's disk block.
- **Recovery:** `rec_commit` makes every block durable; after a crash, `rec_mount` +
  `fs_open` find the filesystem exactly as committed, and fsck is clean.

## 8. fsck: `fs_check`

It checks everything from scratch and returns the number of problems. The tests run it
after every operation.

1. **The directory tree:** entries point at used inodes; `.` and `..` are right; no loops.
2. **Link counts:** the entries found for each inode equal its `nlink`.
3. **The free inode list:** entries are free and unique; every free inode not in the list
   is at or above the remembered inode; the free inode total is right.
4. **The free block list:** following the chain finds every data block no file uses,
   exactly once; free blocks hold no data except the list's own blocks; the free block
   total is right.

## 9. Limits (constants in `fs.h`)

| Limit | Value |
|---|---|
| Inodes | 64 (62 for files and directories after `/`) |
| Inode numbers cached in the superblock (`FS_NICINOD`) | 8 |
| Block numbers cached in the superblock (`FS_NICFREE`) | 12 |
| Largest file | 12 blocks = 1,536 bytes |
| Names per directory | 46 |
| Longest name / path | 27 / 255 characters |
| Data blocks | 991 × 128 bytes |

Not included: rename, permissions and owners, timestamps, indirect blocks, the in-core
inode table with reference counts and locks (there is one process, and the API is by
path), and the buffer cache.

## How this differs from real btrfs

Btrfs has no inode table and no free lists of this kind:
- Inodes and directory entries are items in its FS tree.
- Free space is whatever the extent tree doesn't record as used.
- New inode numbers come from the highest number in use plus one.

This project uses the classic Unix layout and algorithms for files, and btrfs's ideas
(copy-on-write, shared blocks with reference counts, checkpoints, a two-slot superblock)
underneath them.
