/*
 * transaction.c - transactions (fs/btrfs/transaction.c).
 *
 * Every change joins the one running transaction. An operation first
 * RESERVES the metadata space it could need (btrfs_start_transaction), so
 * it fails with -ENOSPC before changing anything rather than half-way. If
 * something still fails after changes were made, the transaction is
 * ABORTED: the filesystem turns read-only, and the disk keeps the last
 * commit.
 *
 * btrfs_commit_transaction:
 *   1. create the pending block groups, run the delayed refs;
 *   2. make the pending snapshots (create_pending_snapshots);
 *   3. write every changed subvolume's root item (commit_fs_roots);
 *   4. until nothing changes: run delayed refs, write block group items,
 *      write the root items of the extent, csum, free space and uuid trees
 *      (each of these changes the others);
 *   5. write every new tree block, with its checksum; flush the disk;
 *   6. write the superblock, pointing at the new root tree, to every
 *      superblock copy; flush. Only now is the transaction durable;
 *   7. space freed in this transaction (pinned) becomes free.
 */
#include <errno.h>

#include "ctree.h"

int writable(const Btrfs *b)
{
    return b->error || b->read_only ? -EROFS : 0;
}

int trans_start(Btrfs *b, Trans *t, unsigned items)
{
    TRY(writable(b));
    t->b = b;
    t->rsv = 0;
    uint64_t bytes = metadata_bytes_for(b, items);
    TRY(reserve_metadata(b, bytes, 1));
    t->rsv = bytes;
    return 0;
}

/* btrfs_start_transaction_fallback_global_rsv: deleting must work even on
 * a full filesystem, so it may use the global reserve. */
int trans_start_fallback_global(Btrfs *b, Trans *t, unsigned items)
{
    int r = trans_start(b, t, items);
    if (r == -ENOSPC) {
        t->b = b;
        t->rsv = 0;
        return 0;
    }
    return r;
}

void trans_end(Trans *t)
{
    if (t->rsv) {
        release_metadata(t->b, t->rsv);
        t->rsv = 0;
    }
}

int trans_abort(Trans *t, int err)
{
    if (!t->b->error)
        t->b->error = err; /* "Transaction aborted": read-only from now on */
    return err;
}

static int commit_inner(Trans *t)
{
    Btrfs *b = t->b;
    BtrfsSuper *sb = &b->super;

    /* The superblock's generation must be its root tree's: every commit
     * writes a new root tree block. */
    TRY(btrfs_cow_root_node(t, b->tree_root));
    TRY(create_pending_block_groups(t));
    TRY(run_delayed_refs(t));
    TRY(create_pending_snapshots(t));
    TRY(commit_fs_roots(t));
    for (;;) {
        int changed = 0;
        TRY(create_pending_block_groups(t));
        TRY(run_delayed_refs(t));
        TRY(write_dirty_block_groups(t));
        TRY(commit_cowonly_roots(t, &changed));
        TRY(commit_fs_roots(t));
        if (!changed && b->delayed.count == 0 && !any_dirty_block_groups(b))
            break;
    }

    uint64_t allocated = 0;
    for (size_t i = 0; i < b->nbgs; i++)
        allocated += b->bgs[i]->len;
    sb->generation = b->transid;
    sb->root = b->tree_root->node->start;
    sb->root_level = (uint8_t)eb_level(b->tree_root->node);
    sb->bytes_used = total_bytes_used(b);
    sb->dev_item.bytes_used = allocated;
    sb->uuid_tree_generation = b->transid;
    backup_super_roots(b);

    TRY(write_dirty_tree_blocks(b));
    TRY(disk_flush(b->disk));
    TRY(write_all_supers(b));
    return 0;
}

int btrfs_commit(Btrfs *b)
{
    if (b->error)
        return -EROFS;
    if (b->read_only || b->in_commit)
        return 0;
    if (!b->trans_dirty && !b->pending_snaps)
        return 0;

    Trans t = { b, 0 };
    b->in_commit = 1;
    int r = commit_inner(&t);
    b->in_commit = 0;
    if (r < 0)
        return trans_abort(&t, r);

    b->generation = b->transid;
    b->transid++;
    for (Root *root = b->roots; root; root = root->next) {
        root->commit_root = root->node ? root->node->start : 0;
        root->force_cow = 0;
    }
    unpin_extents(b);
    eb_free_stale(b);
    b->trans_dirty = 0;
    b->stats.commits++;
    refill_global_rsv(b);
    /* The cleaner: deleted subvolumes are dropped in the next transaction. */
    return drop_dead_roots(b);
}
