# Feature: backup (send / receive) (to do)

**Owner:** _[name]_

This is modelled on `btrfs send` / `btrfs receive`. It turns the difference between
two trees into a stream of changes, and applies that stream to another volume.

- **Full backup:** every page of a tree.
- **Incremental backup:** only the pages that differ between a base tree and a newer
  one. Sharing makes this cheap: if both trees point at the **same block number**
  (a leaf or a data block), nothing below it changed, so skip it.

## Suggested files

```text
include/backup.h         int backup_send(Volume *v, const char *base, const char *target, FILE *out);
                         int backup_receive(Volume *v, FILE *in);
src/backup.c
tests/test_backup.c
demo/demo_backup.c       (optional)
```

## Core API you will use

- `vol_root_find(v, name, &root)` gets the base and target trees. They are usually
  checkpoints: `ckpt_create` from `features/checkpoint`.
- `cowtree_get(vol_tree(v), root, page)` compares block numbers page by page. Equal
  numbers mean unchanged.
- `vol_read_tree(v, root, page, buf, len)` reads the contents to send.
- On receive, use `vol_write` / `vol_erase`, then `vol_root_add` to record the
  received tree as the base for the next incremental.

To skip shared subtrees without reading every page, you may need a small new core
function, such as `cowtree_leaf(root, i)`. Add it to `core/include/cowtree.h`, and
run `make test` for all features.
