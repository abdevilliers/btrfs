/*
 * root_tree.c - the root tree, subvolumes and snapshots (fs/btrfs/
 * root-tree.c, transaction.c create_pending_snapshot, ioctl.c, and
 * extent-tree.c btrfs_drop_snapshot).
 *
 * The root tree holds, for every other tree, a ROOT_ITEM with its root
 * block. A subvolume is an FS tree: its ROOT_ITEM (id, ROOT_ITEM, 0 or
 * the snapshot's creation transid), a ROOT_REF / ROOT_BACKREF pair naming
 * the directory that holds it, and that directory entry, whose location
 * is (id, ROOT_ITEM, -1).
 *
 * A SNAPSHOT copies just the subvolume's root block: the copy points at the
 * same children, which each gain a reference (TREE_BLOCK_REF from the new
 * subvolume). Everything below is shared until copy-on-write separates it.
 *
 * DELETING a subvolume removes its name and sets its root item's refs to 0
 * with an ORPHAN_ITEM; after the commit the cleaner walks the tree and
 * drops one reference to each block (btrfs_drop_snapshot), freeing what
 * nothing else uses.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ctree.h"

/* ------------------------------------------------------------- roots */

static int is_fs_tree(uint64_t id)
{
    return id == BTRFS_FS_TREE_OBJECTID || (id >= BTRFS_FIRST_FREE_OBJECTID && id <= BTRFS_LAST_FREE_OBJECTID);
}

Root *root_new(Btrfs *b, uint64_t objectid)
{
    Root *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->fs_info = b;
    r->objectid = objectid;
    r->shareable = is_fs_tree(objectid);
    r->next = b->roots;
    b->roots = r;
    return r;
}

static void root_unlink(Btrfs *b, Root *r)
{
    for (Root **pp = &b->roots; *pp; pp = &(*pp)->next)
        if (*pp == r) {
            *pp = r->next;
            break;
        }
    free(r);
}

void roots_destroy(Btrfs *b)
{
    while (b->roots) {
        Root *next = b->roots->next;
        free(b->roots);
        b->roots = next;
    }
}

static int find_root_item(Btrfs *b, uint64_t id, BtrfsKey *key, BtrfsRootItem *item)
{
    Path p;
    BtrfsKey k = mkkey(id, BTRFS_ROOT_ITEM_KEY, (uint64_t)-1);
    if (btrfs_search_back(b, b->tree_root, &k, &p) != 0)
        return -ENOENT;
    item_key(p.nodes[0], p.slots[0], key);
    if (key->objectid != id || key->type != BTRFS_ROOT_ITEM_KEY)
        return -ENOENT;
    uint32_t sz = item_size(p.nodes[0], p.slots[0]);
    memset(item, 0, sizeof *item);
    memcpy(item, item_ptr(p.nodes[0], p.slots[0]), sz < sizeof *item ? sz : sizeof *item);
    return 0;
}

int load_root(Btrfs *b, Root *r)
{
    TRY(read_tree_block(b, r->item.bytenr, r->item.generation, r->item.level, &r->node));
    r->commit_root = r->node->start;
    return 0;
}

Root *get_root(Btrfs *b, uint64_t id)
{
    for (Root *r = b->roots; r; r = r->next)
        if (r->objectid == id)
            return r;
    BtrfsKey k;
    BtrfsRootItem item;
    if (find_root_item(b, id, &k, &item) < 0)
        return NULL;
    Root *r = root_new(b, id);
    if (!r)
        return NULL;
    r->key_offset = k.offset;
    r->item = item;
    if (load_root(b, r) < 0) {
        root_unlink(b, r);
        return NULL;
    }
    return r;
}

Root *get_fs_root(Btrfs *b, uint64_t id)
{
    if (!is_fs_tree(id))
        return NULL;
    Root *r = get_root(b, id);
    return r && r->item.refs ? r : NULL;
}

static void set_root_node_fields(Root *r)
{
    r->item.bytenr = r->node->start;
    r->item.level = (uint8_t)eb_level(r->node);
    r->item.generation = eb_gen(r->node);
    r->item.generation_v2 = r->item.generation;
}

int update_root_item(Trans *t, Root *r)
{
    set_root_node_fields(r);
    BtrfsKey k = mkkey(r->objectid, BTRFS_ROOT_ITEM_KEY, r->key_offset);
    return btrfs_update_item(t, t->b->tree_root, &k, &r->item, sizeof r->item);
}

int insert_root_item(Trans *t, Root *r)
{
    set_root_node_fields(r);
    BtrfsKey k = mkkey(r->objectid, BTRFS_ROOT_ITEM_KEY, r->key_offset);
    return btrfs_insert_item(t, t->b->tree_root, &k, &r->item, sizeof r->item);
}

int create_empty_tree(Trans *t, uint64_t objectid, Root **out)
{
    Btrfs *b = t->b;
    Root *r = root_new(b, objectid);
    if (!r)
        return -ENOMEM;
    TRY(alloc_tree_block(t, r, 0, &r->node));
    r->item.refs = 1;
    r->item.root_dirid = 0;
    r->item.inode.nlink = 1;
    r->item.inode.mode = 040755;
    r->item.inode.generation = 1;
    TRY(insert_root_item(t, r));
    *out = r;
    return 0;
}

/* commit_fs_roots: write the root item of every changed subvolume. */
int commit_fs_roots(Trans *t)
{
    for (Root *r = t->b->roots; r; r = r->next)
        if (r->shareable && r->dirty) {
            r->dirty = 0;
            TRY(update_root_item(t, r));
        }
    return 0;
}

/* commit_cowonly_roots: the extent, csum, free space and uuid trees. */
int commit_cowonly_roots(Trans *t, int *changed)
{
    for (Root *r = t->b->roots; r; r = r->next)
        if (!r->shareable && r != t->b->tree_root && r->dirty) {
            r->dirty = 0;
            *changed = 1;
            TRY(update_root_item(t, r));
        }
    return 0;
}

uint64_t highest_root_objectid(Btrfs *b)
{
    Path p;
    BtrfsKey k = mkkey(BTRFS_LAST_FREE_OBJECTID, 0xff, (uint64_t)-1), f;
    if (btrfs_search_back(b, b->tree_root, &k, &p) != 0)
        return BTRFS_FIRST_FREE_OBJECTID - 1;
    item_key(p.nodes[0], p.slots[0], &f);
    return f.objectid >= BTRFS_FIRST_FREE_OBJECTID ? f.objectid : BTRFS_FIRST_FREE_OBJECTID - 1;
}

/* ------------------------------------------------------- uuid tree */

static void random_uuid(uint8_t *u)
{
    static uint64_t s;
    if (!s)
        s = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)&s ^ 0x9E3779B97F4A7C15ULL;
    for (int i = 0; i < BTRFS_UUID_SIZE; i++) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        u[i] = (uint8_t)s;
    }
    u[6] = (uint8_t)((u[6] & 0x0f) | 0x40); /* version 4 */
    u[8] = (uint8_t)((u[8] & 0x3f) | 0x80);
}


void btrfs_random_uuid(uint8_t *u)
{
    random_uuid(u);
}

static BtrfsKey uuid_key(const uint8_t *u)
{
    return mkkey(get64(u), BTRFS_UUID_KEY_SUBVOL, get64(u + 8));
}

int uuid_tree_add(Trans *t, const uint8_t *u, uint64_t id)
{
    Root *ur = t->b->uuid_root;
    BtrfsKey k = uuid_key(u);
    Path p;
    int r = btrfs_search_slot(t, ur, &k, &p, 8, 1);
    if (r < 0)
        return r;
    if (r == 0) {
        btrfs_extend_item(t, &p, 8);
        put64(item_ptr(p.nodes[0], p.slots[0]) + item_size(p.nodes[0], p.slots[0]) - 8, id);
        return 0;
    }
    return btrfs_insert_item(t, ur, &k, &id, sizeof id);
}

static int uuid_remove(Trans *t, const uint8_t *u, uint64_t id)
{
    Root *ur = t->b->uuid_root;
    BtrfsKey k = uuid_key(u);
    Path p;
    int r = btrfs_search_slot(t, ur, &k, &p, -1, 1);
    if (r < 0)
        return r;
    if (r == 1)
        return 0;
    uint32_t sz = item_size(p.nodes[0], p.slots[0]);
    uint8_t *d = item_ptr(p.nodes[0], p.slots[0]);
    for (uint32_t off = 0; off < sz; off += 8)
        if (get64(d + off) == id) {
            if (sz == 8)
                return btrfs_del_item(t, ur, &p);
            memmove(d + off, d + off + 8, sz - off - 8);
            btrfs_truncate_item(t, &p, sz - 8, 1);
            return 0;
        }
    return 0;
}

/* -------------------------------------------------- root refs */

static int add_root_ref(Trans *t, uint64_t parent, uint64_t child, uint64_t dirid, uint64_t index,
                        const char *name)
{
    size_t len = strlen(name);
    uint8_t buf[sizeof(BtrfsRootRef) + FS_NAME_MAX];
    BtrfsRootRef rr = { dirid, index, (uint16_t)len };
    memcpy(buf, &rr, sizeof rr);
    memcpy(buf + sizeof rr, name, len);
    uint32_t sz = (uint32_t)(sizeof rr + len);
    BtrfsKey k = mkkey(parent, BTRFS_ROOT_REF_KEY, child);
    TRY(btrfs_insert_item(t, t->b->tree_root, &k, buf, sz));
    k = mkkey(child, BTRFS_ROOT_BACKREF_KEY, parent);
    return btrfs_insert_item(t, t->b->tree_root, &k, buf, sz);
}

int find_root_ref(Btrfs *b, uint64_t subvol, uint64_t *parent, uint64_t *dirid, char *name)
{
    Path p;
    BtrfsKey k = mkkey(subvol, BTRFS_ROOT_BACKREF_KEY, 0), f;
    if (btrfs_search_forward(b, b->tree_root, &k, &p) != 0)
        return -ENOENT;
    item_key(p.nodes[0], p.slots[0], &f);
    if (f.objectid != subvol || f.type != BTRFS_ROOT_BACKREF_KEY)
        return -ENOENT;
    BtrfsRootRef rr;
    uint8_t *d = item_ptr(p.nodes[0], p.slots[0]);
    memcpy(&rr, d, sizeof rr);
    *parent = f.offset;
    if (dirid)
        *dirid = rr.dirid;
    if (name) {
        memcpy(name, d + sizeof rr, rr.name_len);
        name[rr.name_len] = '\0';
    }
    return 0;
}

/* ---------------------------------------------------------- the handle */

Btrfs *fs_btrfs(Fs *fs) { return fs->b; }
uint64_t fs_subvol_id(const Fs *fs) { return fs->subvol; }

Root *fs_handle_root(Fs *fs)
{
    return get_fs_root(fs->b, fs->subvol);
}

int fs_root_writable(Fs *fs, Root *root)
{
    TRY(writable(fs->b));
    return root->item.flags & BTRFS_ROOT_SUBVOL_RDONLY ? -EROFS : 0;
}

int fs_is_read_only(Fs *fs)
{
    Root *r = fs_handle_root(fs);
    return !r || writable(fs->b) < 0 || (r->item.flags & BTRFS_ROOT_SUBVOL_RDONLY);
}

static BtrfsKey default_key(void)
{
    return mkkey(BTRFS_ROOT_TREE_DIR_OBJECTID, BTRFS_DIR_ITEM_KEY, btrfs_name_hash("default", 7));
}

uint64_t fs_get_default_subvol(Btrfs *b)
{
    BtrfsKey k = default_key();
    BtrfsDirItem di;
    if (btrfs_lookup(b, b->tree_root, &k, &di, sizeof di, NULL) < 0)
        return BTRFS_FS_TREE_OBJECTID;
    return di.location.objectid;
}

int fs_set_default_subvol(Btrfs *b, uint64_t id)
{
    Trans t;
    Path p;
    BtrfsKey k = default_key();
    if (!get_fs_root(b, id))
        return -ENOENT;
    TRY(trans_start(b, &t, 1));
    int r = btrfs_search_slot(&t, b->tree_root, &k, &p, 0, 1);
    if (r == 0) {
        BtrfsDirItem di;
        uint8_t *d = item_ptr(p.nodes[0], p.slots[0]);
        memcpy(&di, d, sizeof di);
        di.location.objectid = id;
        di.transid = b->transid;
        memcpy(d, &di, sizeof di);
        btrfs_mark_dirty(&t, p.nodes[0]);
    } else if (r > 0) {
        r = -EIO;
    }
    trans_end(&t);
    return r < 0 ? trans_abort(&t, r) : 0;
}

Fs *fs_open_subvol(Btrfs *b, uint64_t id)
{
    if (!get_fs_root(b, id))
        return NULL;
    Fs *fs = malloc(sizeof *fs);
    if (fs) {
        fs->b = b;
        fs->subvol = id;
        b->open_handles++;
    }
    return fs;
}

Fs *fs_open(Btrfs *b)
{
    return fs_open_subvol(b, fs_get_default_subvol(b));
}

void fs_close(Fs *fs)
{
    if (fs) {
        fs->b->open_handles--;
        free(fs);
    }
}

/* ------------------------------------------------------ subvolumes */

void init_subvol_item(Btrfs *b, BtrfsRootItem *it)
{
    memset(it, 0, sizeof *it);
    inode_init(&it->inode, 1, 040755);
    it->inode.size = 3;
    it->inode.nbytes = b->nodesize;
    it->root_dirid = BTRFS_FIRST_FREE_OBJECTID;
    it->refs = 1;
    random_uuid(it->uuid);
    it->ctransid = it->otransid = b->transid;
    it->otime.sec = it->ctime.sec = now_sec();
}

int fs_subvol_create(Fs *fs, const char *path)
{
    Btrfs *b = fs->b;
    Inode dir;
    char name[FS_NAME_MAX + 1];
    BtrfsDiskKey loc;
    uint8_t type;
    Trans t;

    TRY(resolve_parent(fs, path, &dir, name));
    TRY(fs_root_writable(fs, dir.root));
    if (dir_lookup(b, dir, name, &loc, &type) == 0)
        return -EEXIST;
    TRY(trans_start(b, &t, 10));

    uint64_t id = b->free_root_objectid++, index;
    Root *r = root_new(b, id);
    int ret = r ? 0 : -ENOMEM;
    if (!ret)
        ret = alloc_tree_block(&t, r, 0, &r->node);
    if (!ret) {
        init_subvol_item(b, &r->item);
        ret = make_root_dir(&t, r, BTRFS_FIRST_FREE_OBJECTID);
    }
    if (!ret)
        ret = insert_root_item(&t, r);
    BtrfsKey l = mkkey(id, BTRFS_ROOT_ITEM_KEY, (uint64_t)-1);
    if (!ret)
        ret = dir_add_entry(&t, dir, name, &l, BTRFS_FT_DIR, &index);
    if (!ret)
        ret = add_root_ref(&t, dir.root->objectid, id, dir.ino, index, name);
    if (!ret)
        ret = uuid_tree_add(&t, r->item.uuid, id);
    if (r)
        r->dirty = 0;
    trans_end(&t);
    return ret < 0 ? trans_abort(&t, ret) : 0;
}

/* create_pending_snapshot: runs inside the commit. */
static int create_one_snapshot(Trans *t, PendingSnap *ps)
{
    Btrfs *b = t->b;
    Root *src = get_fs_root(b, ps->src_id), *parent = get_fs_root(b, ps->parent_id);
    BtrfsDiskKey loc;
    uint8_t type;

    if (!src || !parent) {
        ps->error = -ENOENT;
        return 0;
    }
    Inode dir = { parent, ps->parent_dir };
    if (dir_lookup(b, dir, ps->name, &loc, &type) == 0) {
        ps->error = -EEXIST;
        return 0;
    }

    uint64_t id = b->free_root_objectid++, index;
    TRY(btrfs_cow_root_node(t, src));
    Root *nr = root_new(b, id);
    if (!nr)
        return -ENOMEM;
    ExtentBuffer *copy;
    TRY(alloc_tree_block(t, nr, eb_level(src->node), &copy));
    memcpy(copy->data, src->node->data, b->nodesize);
    eb_set_bytenr(copy, copy->start);
    eb_set_gen(copy, b->transid);
    eb_set_owner(copy, id);
    eb_set_flags(copy, BTRFS_MIXED_BACKREF_REV << BTRFS_BACKREF_REV_SHIFT);
    btrfs_mark_dirty(t, copy);
    nr->node = copy;
    /* btrfs_copy_root: the copy's children gain a reference from it. */
    TRY(btrfs_inc_ref(t, nr, copy, 0));

    src->item.last_snapshot = b->transid;
    src->dirty = 1;
    src->force_cow = 1; /* from now on, copy even this transaction's blocks */

    nr->item = src->item;
    nr->item.bytes_used = b->nodesize;
    nr->key_offset = b->transid;
    nr->item.refs = 1;
    nr->item.flags = ps->readonly ? BTRFS_ROOT_SUBVOL_RDONLY : 0;
    memset(&nr->item.drop_progress, 0, sizeof nr->item.drop_progress);
    nr->item.drop_level = 0;
    memcpy(nr->item.parent_uuid, src->item.uuid, BTRFS_UUID_SIZE);
    random_uuid(nr->item.uuid);
    memset(nr->item.received_uuid, 0, BTRFS_UUID_SIZE);
    nr->item.ctransid = nr->item.otransid = b->transid;
    nr->item.stransid = nr->item.rtransid = 0;
    nr->item.otime.sec = now_sec();
    TRY(insert_root_item(t, nr));

    BtrfsKey l = mkkey(id, BTRFS_ROOT_ITEM_KEY, (uint64_t)-1);
    TRY(dir_add_entry(t, dir, ps->name, &l, BTRFS_FT_DIR, &index));
    TRY(add_root_ref(t, parent->objectid, id, dir.ino, index, ps->name));
    TRY(uuid_tree_add(t, nr->item.uuid, id));
    ps->new_id = id;
    return 0;
}

int create_pending_snapshots(Trans *t)
{
    for (PendingSnap *ps = t->b->pending_snaps; ps; ps = ps->next)
        if (!ps->new_id && !ps->error)
            TRY(create_one_snapshot(t, ps));
    return 0;
}

int fs_subvol_snapshot(Fs *fs, const char *src, const char *dst, int readonly, uint64_t *id)
{
    Btrfs *b = fs->b;
    Inode s, dir;
    char name[FS_NAME_MAX + 1];
    BtrfsDiskKey loc;
    uint8_t type;

    TRY(writable(b));
    TRY(resolve_path(fs, src, &s));
    if (s.ino != BTRFS_FIRST_FREE_OBJECTID)
        return -EINVAL; /* not the root of a subvolume */
    TRY(resolve_parent(fs, dst, &dir, name));
    TRY(fs_root_writable(fs, dir.root));
    if (dir_lookup(b, dir, name, &loc, &type) == 0)
        return -EEXIST;

    PendingSnap *ps = calloc(1, sizeof *ps);
    if (!ps)
        return -ENOMEM;
    ps->src_id = s.root->objectid;
    ps->parent_id = dir.root->objectid;
    ps->parent_dir = dir.ino;
    strcpy(ps->name, name);
    ps->readonly = readonly;
    ps->next = b->pending_snaps;
    b->pending_snaps = ps;
    b->trans_dirty = 1;

    int r = btrfs_commit(b);
    for (PendingSnap **pp = &b->pending_snaps; *pp; pp = &(*pp)->next)
        if (*pp == ps) {
            *pp = ps->next;
            break;
        }
    if (r == 0)
        r = ps->error;
    if (r == 0 && id)
        *id = ps->new_id;
    free(ps);
    return r;
}

int fs_subvol_delete(Fs *fs, const char *path)
{
    Btrfs *b = fs->b;
    Inode dir;
    char name[FS_NAME_MAX + 1];
    BtrfsDiskKey loc;
    uint8_t type;
    Path p;
    Trans t;

    TRY(resolve_parent(fs, path, &dir, name));
    TRY(fs_root_writable(fs, dir.root));
    TRY(dir_lookup(b, dir, name, &loc, &type));
    if (loc.type != BTRFS_ROOT_ITEM_KEY)
        return -EINVAL; /* not a subvolume */
    uint64_t id = loc.objectid;
    if (id == BTRFS_FS_TREE_OBJECTID || id == fs_get_default_subvol(b))
        return -EPERM;
    Root *r = get_fs_root(b, id);
    if (!r)
        return -ENOENT;
    BtrfsKey k = mkkey(id, BTRFS_ROOT_REF_KEY, 0), f;
    if (btrfs_search_forward(b, b->tree_root, &k, &p) == 0) {
        item_key(p.nodes[0], p.slots[0], &f);
        if (f.objectid == id && f.type == BTRFS_ROOT_REF_KEY)
            return -ENOTEMPTY; /* it holds other subvolumes */
    }
    BtrfsRootRef rr;
    k = mkkey(dir.root->objectid, BTRFS_ROOT_REF_KEY, id);
    TRY(btrfs_lookup(b, b->tree_root, &k, &rr, sizeof rr, NULL));

    TRY(trans_start_fallback_global(b, &t, 6));
    int ret = dir_del_entry(&t, dir, name, rr.sequence);
    if (!ret)
        ret = btrfs_delete_key(&t, b->tree_root, &k);
    BtrfsKey bk = mkkey(id, BTRFS_ROOT_BACKREF_KEY, dir.root->objectid);
    if (!ret)
        ret = btrfs_delete_key(&t, b->tree_root, &bk);
    if (!ret)
        ret = uuid_remove(&t, r->item.uuid, id);
    if (!ret) {
        r->item.refs = 0;
        memset(&r->item.drop_progress, 0, sizeof r->item.drop_progress);
        ret = update_root_item(&t, r);
    }
    BtrfsKey ok = mkkey(BTRFS_ORPHAN_OBJECTID, BTRFS_ORPHAN_ITEM_KEY, id);
    if (!ret)
        ret = btrfs_insert_item(&t, b->tree_root, &ok, NULL, 0);
    r->dirty = 0;
    trans_end(&t);
    return ret < 0 ? trans_abort(&t, ret) : 0;
}

int fs_subvol_lookup(Fs *fs, const char *path, uint64_t *id)
{
    Inode in;
    TRY(resolve_path(fs, path, &in));
    if (in.ino != BTRFS_FIRST_FREE_OBJECTID)
        return -EINVAL;
    *id = in.root->objectid;
    return 0;
}

int fs_subvol_info(Btrfs *b, uint64_t id, FsSubvolInfo *info)
{
    Root *r = get_fs_root(b, id);
    if (!r)
        return -ENOENT;
    memset(info, 0, sizeof *info);
    info->id = id;
    if (id != BTRFS_FS_TREE_OBJECTID)
        find_root_ref(b, id, &info->parent_id, &info->dirid, info->name);
    info->read_only = (r->item.flags & BTRFS_ROOT_SUBVOL_RDONLY) != 0;
    info->otransid = r->item.otransid;
    info->generation = r->item.generation;
    memcpy(info->uuid, r->item.uuid, BTRFS_UUID_SIZE);
    memcpy(info->parent_uuid, r->item.parent_uuid, BTRFS_UUID_SIZE);
    return 0;
}

int fs_subvol_list(Btrfs *b, fs_subvol_fn fn, void *arg)
{
    Path p;
    BtrfsKey k = mkkey(BTRFS_FIRST_FREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, 0), f;
    uint64_t *ids = NULL;
    size_t n = 0, cap = 0;
    int r = btrfs_search_forward(b, b->tree_root, &k, &p);
    while (r == 0) {
        item_key(p.nodes[0], p.slots[0], &f);
        if (f.objectid > BTRFS_LAST_FREE_OBJECTID)
            break;
        if (f.type == BTRFS_ROOT_ITEM_KEY) {
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                uint64_t *ni = realloc(ids, cap * sizeof *ni);
                if (!ni) {
                    free(ids);
                    return -ENOMEM;
                }
                ids = ni;
            }
            ids[n++] = f.objectid;
        }
        r = btrfs_next_item(b, &p);
    }
    for (size_t i = 0; i < n && r >= 0; i++) {
        FsSubvolInfo info;
        if (fs_subvol_info(b, ids[i], &info) == 0)
            fn(arg, &info);
    }
    free(ids);
    return r < 0 ? r : 0;
}

/* -------------------------------------------------- dropping a subvolume */

static int ref_of(ExtentBuffer *parent, uint64_t parent_flags, uint64_t root_id, uint64_t *rid,
                  uint64_t *pbytenr)
{
    if (!parent) {
        *rid = root_id;
        *pbytenr = 0;
    } else if (parent_flags & BTRFS_BLOCK_FLAG_FULL_BACKREF) {
        *rid = 0;
        *pbytenr = parent->start;
    } else {
        *rid = eb_owner(parent);
        *pbytenr = 0;
    }
    return 0;
}

/* The UPDATE_BACKREF stage: before letting go of a block another tree
 * still uses, every block of this tree under it switches its children to
 * SHARED refs, because refs named after a dead tree could never be
 * matched by a later copy-on-write. */
static int update_backref(Trans *t, Root *r, ExtentBuffer *eb)
{
    Btrfs *b = t->b;
    uint64_t refs, flags;
    TRY(lookup_extent_info(b, eb->start, eb_level(eb), 0, &refs, &flags));
    if (!(flags & BTRFS_BLOCK_FLAG_FULL_BACKREF)) {
        TRY(btrfs_inc_ref(t, r, eb, 1));
        TRY(btrfs_dec_ref(t, r, eb, 0));
        TRY(set_extent_flags(t, eb, BTRFS_BLOCK_FLAG_FULL_BACKREF));
    }
    if (eb_level(eb) == 0)
        return 0;
    for (int i = 0; i < (int)eb_nritems(eb); i++) {
        if (node_ptr_gen(eb, i) <= r->key_offset)
            continue; /* older than this tree: not its block */
        ExtentBuffer *c;
        TRY(read_tree_block(b, node_blockptr(eb, i), node_ptr_gen(eb, i), eb_level(eb) - 1, &c));
        if (eb_owner(c) == r->objectid)
            TRY(update_backref(t, r, c));
    }
    return 0;
}

/* walk_down / walk_up of btrfs_drop_snapshot, as a recursion: drop this
 * tree's reference to a block; if it was the only one, first drop the
 * block's own references to its children (or data extents). */
static int walk_drop(Trans *t, Root *r, uint64_t bytenr, uint64_t gen, int level, ExtentBuffer *parent,
                     uint64_t parent_flags)
{
    Btrfs *b = t->b;
    uint64_t refs, flags, rid, pb;
    ExtentBuffer *eb;

    ref_of(parent, parent_flags, r->objectid, &rid, &pb);
    TRY(lookup_extent_info(b, bytenr, level, 0, &refs, &flags));
    if (refs == 0)
        return -EIO;
    if (refs > 1) {
        if (gen > r->key_offset) {
            TRY(read_tree_block(b, bytenr, gen, level, &eb));
            if (eb_owner(eb) == r->objectid)
                TRY(update_backref(t, r, eb));
        }
        return drop_tree_ref(t, bytenr, level, rid, pb);
    }
    TRY(read_tree_block(b, bytenr, gen, level, &eb));
    if (level > 0) {
        for (int i = 0; i < (int)eb_nritems(eb); i++)
            TRY(walk_drop(t, r, node_blockptr(eb, i), node_ptr_gen(eb, i), level - 1, eb, flags));
    } else {
        TRY(btrfs_dec_ref(t, r, eb, (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF) != 0));
    }
    return drop_tree_ref(t, bytenr, level, rid, pb);
}

static int drop_snapshot(Trans *t, Root *r)
{
    Btrfs *b = t->b;
    TRY(walk_drop(t, r, r->node->start, eb_gen(r->node), eb_level(r->node), NULL, 0));
    BtrfsKey k = mkkey(r->objectid, BTRFS_ROOT_ITEM_KEY, r->key_offset);
    TRY(btrfs_delete_key(t, b->tree_root, &k));
    k = mkkey(BTRFS_ORPHAN_OBJECTID, BTRFS_ORPHAN_ITEM_KEY, r->objectid);
    TRY(btrfs_delete_key(t, b->tree_root, &k));
    root_unlink(b, r);
    return 0;
}

/* The cleaner: drop every deleted subvolume (btrfs_clean_one_deleted_snapshot). */
int drop_dead_roots(Btrfs *b)
{
    if (writable(b) < 0)
        return 0;
    for (;;) {
        Path p;
        BtrfsKey k = mkkey(BTRFS_ORPHAN_OBJECTID, BTRFS_ORPHAN_ITEM_KEY, 0), f;
        if (btrfs_search_forward(b, b->tree_root, &k, &p) != 0)
            return 0;
        item_key(p.nodes[0], p.slots[0], &f);
        if (f.objectid != BTRFS_ORPHAN_OBJECTID || f.type != BTRFS_ORPHAN_ITEM_KEY)
            return 0;
        Root *r = get_root(b, f.offset);
        Trans t = { b, 0 };
        if (!r || r->item.refs != 0)
            return 0; /* not a dead subvolume: leave it */
        int ret = drop_snapshot(&t, r);
        if (ret < 0)
            return trans_abort(&t, ret);
    }
}
