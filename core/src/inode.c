/*
 * inode.c - inodes and directories in an FS tree (fs/btrfs/inode.c,
 * dir-item.c, inode-item.c).
 *
 * A file or directory is a set of items keyed by its inode number:
 *
 *   (ino, INODE_ITEM, 0)          size, nlink, mode, times, nbytes
 *   (ino, INODE_REF, parent dir)  { index, name } for each name it has there
 *   (ino, EXTENT_DATA, offset)    its data (file.c)
 *
 * and a directory holds, for each entry, two items:
 *
 *   (dir, DIR_ITEM, crc32c(name))  { location, type, name }: lookup by name
 *   (dir, DIR_INDEX, index)        the same, in creation order: readdir
 *
 * "." and ".." are not stored. A directory's size is twice the length of
 * all its names; its nlink is always 1. New inode numbers are the highest
 * in the tree plus one.
 */
#include <errno.h>
#include <stdlib.h>
#include <time.h>

#include "ctree.h"

#define MODE_DIR 0040000u
#define MODE_REG 0100000u
#define MODE_FMT 0170000u

uint64_t now_sec(void)
{
    return (uint64_t)time(NULL);
}

static int is_dir(const BtrfsInodeItem *ii)
{
    return (ii->mode & MODE_FMT) == MODE_DIR;
}

void inode_init(BtrfsInodeItem *ii, uint64_t transid, uint32_t mode)
{
    memset(ii, 0, sizeof *ii);
    ii->generation = transid;
    ii->transid = transid;
    ii->nlink = 1;
    ii->mode = mode;
    uint64_t now = now_sec();
    ii->atime.sec = ii->ctime.sec = ii->mtime.sec = ii->otime.sec = now;
}

int inode_get(Btrfs *b, Inode in, BtrfsInodeItem *ii)
{
    BtrfsKey k = mkkey(in.ino, BTRFS_INODE_ITEM_KEY, 0);
    return btrfs_lookup(b, in.root, &k, ii, sizeof *ii, NULL);
}

int inode_put(Trans *t, Inode in, const BtrfsInodeItem *ii)
{
    BtrfsKey k = mkkey(in.ino, BTRFS_INODE_ITEM_KEY, 0);
    return btrfs_update_item(t, in.root, &k, ii, sizeof *ii);
}

static void touch(BtrfsInodeItem *ii, uint64_t transid)
{
    ii->transid = transid;
    ii->sequence++;
    ii->mtime.sec = ii->ctime.sec = now_sec();
}

int next_objectid(Btrfs *b, Root *root, uint64_t *ino)
{
    if (!root->highest_objectid) {
        Path p;
        BtrfsKey k = mkkey(BTRFS_LAST_FREE_OBJECTID, 0xff, (uint64_t)-1), f;
        root->highest_objectid = BTRFS_FIRST_FREE_OBJECTID;
        if (btrfs_search_back(b, root, &k, &p) == 0) {
            item_key(p.nodes[0], p.slots[0], &f);
            if (f.objectid > root->highest_objectid)
                root->highest_objectid = f.objectid;
        }
    }
    *ino = ++root->highest_objectid;
    return 0;
}

/* --------------------------------------------- names inside an item */

/* Offset of the entry for `name` in a DIR_ITEM's data, or -1. */
static int64_t find_dir_entry(const uint8_t *d, uint32_t size, const char *name, size_t len)
{
    for (uint32_t off = 0; off + sizeof(BtrfsDirItem) <= size;) {
        BtrfsDirItem di;
        memcpy(&di, d + off, sizeof di);
        if (di.name_len == len && memcmp(d + off + sizeof di, name, len) == 0)
            return off;
        off += (uint32_t)sizeof di + di.name_len + di.data_len;
    }
    return -1;
}

/* Offset of the entry for `name` in an INODE_REF's data, or -1. */
static int64_t find_ref_entry(const uint8_t *d, uint32_t size, const char *name, size_t len)
{
    for (uint32_t off = 0; off + sizeof(BtrfsInodeRef) <= size;) {
        BtrfsInodeRef ir;
        memcpy(&ir, d + off, sizeof ir);
        if (ir.name_len == len && memcmp(d + off + sizeof ir, name, len) == 0)
            return off;
        off += (uint32_t)sizeof ir + ir.name_len;
    }
    return -1;
}

/* Add `entry` to the item at `key`: append to it if it exists. */
static int add_to_item(Trans *t, Root *root, const BtrfsKey *key, const uint8_t *entry, uint32_t esize)
{
    Path p;
    int r = btrfs_search_slot(t, root, key, &p, (int)esize, 1);
    if (r < 0)
        return r;
    if (r == 0) {
        btrfs_extend_item(t, &p, esize);
        memcpy(item_ptr(p.nodes[0], p.slots[0]) + item_size(p.nodes[0], p.slots[0]) - esize, entry, esize);
        return 0;
    }
    return btrfs_insert_item(t, root, key, entry, esize);
}

/* Remove esize bytes at `off` from the item at `key` (or the whole item). */
static int cut_from_item(Trans *t, Root *root, const BtrfsKey *key, uint32_t off, uint32_t esize)
{
    Path p;
    int r = btrfs_search_slot(t, root, key, &p, -1, 1);
    if (r < 0)
        return r;
    if (r == 1)
        return -ENOENT;
    uint32_t size = item_size(p.nodes[0], p.slots[0]);
    if (esize == size)
        return btrfs_del_item(t, root, &p);
    uint8_t *d = item_ptr(p.nodes[0], p.slots[0]);
    memmove(d + off, d + off + esize, size - off - esize);
    btrfs_truncate_item(t, &p, size - esize, 1);
    return 0;
}

static int read_item(Btrfs *b, Root *root, const BtrfsKey *k, uint8_t **data, uint32_t *size)
{
    Path p;
    int r = btrfs_search_forward(b, root, k, &p);
    if (r != 0)
        return r < 0 ? r : -ENOENT;
    BtrfsKey f;
    item_key(p.nodes[0], p.slots[0], &f);
    if (btrfs_comp_keys(&f, k) != 0)
        return -ENOENT;
    *data = item_ptr(p.nodes[0], p.slots[0]);
    *size = item_size(p.nodes[0], p.slots[0]);
    return 0;
}

/* ----------------------------------------------------------- directories */

int dir_lookup(Btrfs *b, Inode dir, const char *name, BtrfsDiskKey *location, uint8_t *type)
{
    size_t len = strlen(name);
    BtrfsKey k = mkkey(dir.ino, BTRFS_DIR_ITEM_KEY, btrfs_name_hash(name, len));
    uint8_t *d;
    uint32_t size;
    TRY(read_item(b, dir.root, &k, &d, &size));
    int64_t off = find_dir_entry(d, size, name, len);
    if (off < 0)
        return -ENOENT;
    BtrfsDirItem di;
    memcpy(&di, d + off, sizeof di);
    *location = di.location;
    *type = di.type;
    return 0;
}

static uint64_t next_dir_index(Btrfs *b, Inode dir)
{
    Path p;
    BtrfsKey k = mkkey(dir.ino, BTRFS_DIR_INDEX_KEY, (uint64_t)-1), f;
    if (btrfs_search_back(b, dir.root, &k, &p) == 0) {
        item_key(p.nodes[0], p.slots[0], &f);
        if (f.objectid == dir.ino && f.type == BTRFS_DIR_INDEX_KEY)
            return f.offset + 1;
    }
    return 2; /* 0 and 1 are "." and ".." */
}

int dir_add_entry(Trans *t, Inode dir, const char *name, const BtrfsKey *location, uint8_t type,
                  uint64_t *index)
{
    Btrfs *b = t->b;
    size_t len = strlen(name);
    uint8_t entry[sizeof(BtrfsDirItem) + FS_NAME_MAX];
    BtrfsDirItem di;
    BtrfsInodeItem ii;

    di.location.objectid = location->objectid;
    di.location.type = location->type;
    di.location.offset = location->offset;
    di.transid = b->transid;
    di.data_len = 0;
    di.name_len = (uint16_t)len;
    di.type = type;
    memcpy(entry, &di, sizeof di);
    memcpy(entry + sizeof di, name, len);
    uint32_t esize = (uint32_t)(sizeof di + len);

    *index = next_dir_index(b, dir);
    BtrfsKey k = mkkey(dir.ino, BTRFS_DIR_ITEM_KEY, btrfs_name_hash(name, len));
    TRY(add_to_item(t, dir.root, &k, entry, esize));
    k = mkkey(dir.ino, BTRFS_DIR_INDEX_KEY, *index);
    TRY(btrfs_insert_item(t, dir.root, &k, entry, esize));

    TRY(inode_get(b, dir, &ii));
    ii.size += 2 * len;
    touch(&ii, b->transid);
    return inode_put(t, dir, &ii);
}

int dir_del_entry(Trans *t, Inode dir, const char *name, uint64_t index)
{
    Btrfs *b = t->b;
    size_t len = strlen(name);
    BtrfsKey k = mkkey(dir.ino, BTRFS_DIR_ITEM_KEY, btrfs_name_hash(name, len));
    uint8_t *d;
    uint32_t size;
    BtrfsInodeItem ii;

    TRY(read_item(b, dir.root, &k, &d, &size));
    int64_t off = find_dir_entry(d, size, name, len);
    if (off < 0)
        return -ENOENT;
    TRY(cut_from_item(t, dir.root, &k, (uint32_t)off, (uint32_t)(sizeof(BtrfsDirItem) + len)));
    k = mkkey(dir.ino, BTRFS_DIR_INDEX_KEY, index);
    TRY(btrfs_delete_key(t, dir.root, &k));

    TRY(inode_get(b, dir, &ii));
    ii.size -= 2 * len;
    touch(&ii, b->transid);
    return inode_put(t, dir, &ii);
}

int inode_ref_add(Trans *t, Inode in, uint64_t parent, const char *name, uint64_t index)
{
    size_t len = strlen(name);
    uint8_t entry[sizeof(BtrfsInodeRef) + FS_NAME_MAX];
    BtrfsInodeRef ir = { index, (uint16_t)len };
    memcpy(entry, &ir, sizeof ir);
    memcpy(entry + sizeof ir, name, len);
    BtrfsKey k = mkkey(in.ino, BTRFS_INODE_REF_KEY, parent);
    return add_to_item(t, in.root, &k, entry, (uint32_t)(sizeof ir + len));
}

int inode_ref_del(Trans *t, Inode in, uint64_t parent, const char *name, uint64_t *index)
{
    size_t len = strlen(name);
    BtrfsKey k = mkkey(in.ino, BTRFS_INODE_REF_KEY, parent);
    uint8_t *d;
    uint32_t size;
    TRY(read_item(t->b, in.root, &k, &d, &size));
    int64_t off = find_ref_entry(d, size, name, len);
    if (off < 0)
        return -ENOENT;
    BtrfsInodeRef ir;
    memcpy(&ir, d + off, sizeof ir);
    *index = ir.index;
    return cut_from_item(t, in.root, &k, (uint32_t)off, (uint32_t)(sizeof ir + len));
}

/* The root directory of a new subvolume: inode 256, whose INODE_REF names
 * itself "..". */
int make_root_dir(Trans *t, Root *root, uint64_t ino)
{
    BtrfsInodeItem ii;
    inode_init(&ii, t->b->transid, MODE_DIR | 0755);
    BtrfsKey k = mkkey(ino, BTRFS_INODE_ITEM_KEY, 0);
    TRY(btrfs_insert_item(t, root, &k, &ii, sizeof ii));
    if (root->highest_objectid < ino)
        root->highest_objectid = ino;
    return inode_ref_add(t, (Inode){ root, ino }, ino, "..", 0);
}

int dir_is_empty(Btrfs *b, Inode dir)
{
    BtrfsInodeItem ii;
    return inode_get(b, dir, &ii) == 0 && ii.size == 0;
}

/* ------------------------------------------------------------ path lookup */

/* The subvolume a directory entry in `root` leads to. A snapshot copies
 * the entries of subvolumes nested in its source, but they are not its
 * own (no ROOT_REF from it): btrfs shows those as empty directories; here
 * they are skipped. */
Root *subvol_entry_root(Btrfs *b, Root *root, uint64_t sub)
{
    BtrfsKey k = mkkey(root->objectid, BTRFS_ROOT_REF_KEY, sub);
    if (btrfs_lookup(b, b->tree_root, &k, NULL, 0, NULL) < 0)
        return NULL;
    return get_fs_root(b, sub);
}

static int next_name(const char **p, char *name)
{
    const char *s = *p;
    size_t n = 0;
    while (*s == '/')
        s++;
    if (!*s) {
        *p = s;
        return 0;
    }
    while (s[n] && s[n] != '/')
        n++;
    if (n > FS_NAME_MAX)
        return -ENAMETOOLONG;
    memcpy(name, s, n);
    name[n] = '\0';
    *p = s + n;
    return 1;
}

/* ".." of an inode: its INODE_REF's parent, or across a subvolume root. */
static int parent_of(Fs *fs, Inode *cur)
{
    Btrfs *b = fs->b;
    if (cur->ino == BTRFS_FIRST_FREE_OBJECTID) {
        uint64_t parent, dirid;
        if (cur->root->objectid == fs->subvol || find_root_ref(b, cur->root->objectid, &parent, &dirid, NULL) < 0)
            return 0; /* the top of this handle: ".." is itself */
        Root *pr = get_fs_root(b, parent);
        if (!pr)
            return -ENOENT;
        cur->root = pr;
        cur->ino = dirid;
        return 0;
    }
    Path p;
    BtrfsKey k = mkkey(cur->ino, BTRFS_INODE_REF_KEY, 0), f;
    if (btrfs_search_forward(b, cur->root, &k, &p) != 0)
        return -EIO;
    item_key(p.nodes[0], p.slots[0], &f);
    if (f.objectid != cur->ino || f.type != BTRFS_INODE_REF_KEY)
        return -EIO;
    cur->ino = f.offset;
    return 0;
}

static int check_path(const char *path)
{
    if (!path || path[0] != '/')
        return -EINVAL;
    if (strlen(path) > FS_PATH_MAX)
        return -ENAMETOOLONG;
    return 0;
}

int resolve_path(Fs *fs, const char *path, Inode *out)
{
    Btrfs *b = fs->b;
    char name[FS_NAME_MAX + 1];
    int r;
    Root *root = fs_handle_root(fs);
    if (!root)
        return -ESTALE; /* the subvolume was deleted */
    TRY(check_path(path));
    Inode cur = { root, BTRFS_FIRST_FREE_OBJECTID };

    while ((r = next_name(&path, name)) == 1) {
        if (strcmp(name, ".") == 0)
            continue;
        if (strcmp(name, "..") == 0) {
            TRY(parent_of(fs, &cur));
            continue;
        }
        BtrfsInodeItem ii;
        BtrfsDiskKey loc;
        uint8_t type;
        TRY(inode_get(b, cur, &ii));
        if (!is_dir(&ii))
            return -ENOTDIR;
        TRY(dir_lookup(b, cur, name, &loc, &type));
        if (loc.type == BTRFS_ROOT_ITEM_KEY) {
            Root *sub = subvol_entry_root(b, cur.root, loc.objectid);
            if (!sub)
                return -ENOENT;
            cur.root = sub;
            cur.ino = BTRFS_FIRST_FREE_OBJECTID;
        } else {
            cur.ino = loc.objectid;
        }
    }
    if (r < 0)
        return r;
    *out = cur;
    return 0;
}

int resolve_parent(Fs *fs, const char *path, Inode *parent, char *name)
{
    char buf[FS_PATH_MAX + 1];
    BtrfsInodeItem ii;
    TRY(check_path(path));
    strcpy(buf, path);
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/')
        buf[--n] = '\0';
    char *slash = strrchr(buf, '/');
    const char *last = slash + 1;
    if (!*last || strcmp(last, ".") == 0 || strcmp(last, "..") == 0)
        return -EINVAL;
    if (strlen(last) > FS_NAME_MAX)
        return -ENAMETOOLONG;
    strcpy(name, last);
    if (slash == buf)
        slash[1] = '\0';
    else
        *slash = '\0';
    TRY(resolve_path(fs, buf, parent));
    TRY(inode_get(fs->b, *parent, &ii));
    return is_dir(&ii) ? 0 : -ENOTDIR;
}

/* --------------------------------------------------------------- stat */

static int stat_inode(Btrfs *b, Inode in, FsStat *st)
{
    BtrfsInodeItem ii;
    TRY(inode_get(b, in, &ii));
    memset(st, 0, sizeof *st);
    st->ino = in.ino;
    st->subvol = in.root->objectid;
    st->type = is_dir(&ii) ? FS_DIR : FS_FILE;
    st->nlink = ii.nlink;
    st->size = ii.size;
    st->nbytes = ii.nbytes;
    st->generation = ii.generation;
    st->transid = ii.transid;
    return 0;
}

int fs_stat(Fs *fs, const char *path, FsStat *st)
{
    Inode in;
    TRY(resolve_path(fs, path, &in));
    return stat_inode(fs->b, in, st);
}

/* --------------------------------------------- create, mkdir, link */

static int make_node(Fs *fs, const char *path, int dir_type)
{
    Btrfs *b = fs->b;
    Inode dir;
    char name[FS_NAME_MAX + 1];
    BtrfsDiskKey loc;
    uint8_t type;
    Trans t;
    BtrfsInodeItem ii;
    uint64_t ino, index;

    TRY(resolve_parent(fs, path, &dir, name));
    TRY(fs_root_writable(fs, dir.root));
    if (dir_lookup(b, dir, name, &loc, &type) == 0)
        return -EEXIST;
    /* btrfs_create: inode item, inode ref, two dir items, the parent. */
    TRY(trans_start(b, &t, 5));
    int r = next_objectid(b, dir.root, &ino);
    inode_init(&ii, b->transid, dir_type ? MODE_DIR | 0755 : MODE_REG | 0644);
    BtrfsKey k = mkkey(ino, BTRFS_INODE_ITEM_KEY, 0);
    if (!r)
        r = btrfs_insert_item(&t, dir.root, &k, &ii, sizeof ii);
    if (!r)
        r = dir_add_entry(&t, dir, name, &k, dir_type ? BTRFS_FT_DIR : BTRFS_FT_REG_FILE, &index);
    if (!r)
        r = inode_ref_add(&t, (Inode){ dir.root, ino }, dir.ino, name, index);
    trans_end(&t);
    return r < 0 ? trans_abort(&t, r) : 0;
}

int fs_create(Fs *fs, const char *path)
{
    return make_node(fs, path, 0);
}

int fs_mkdir(Fs *fs, const char *path)
{
    return make_node(fs, path, 1);
}

int fs_link(Fs *fs, const char *existing, const char *newpath)
{
    Btrfs *b = fs->b;
    Inode in, dir;
    char name[FS_NAME_MAX + 1];
    BtrfsDiskKey loc;
    uint8_t type;
    BtrfsInodeItem ii;
    Trans t;
    uint64_t index;

    TRY(resolve_path(fs, existing, &in));
    TRY(inode_get(b, in, &ii));
    if (is_dir(&ii))
        return -EPERM;
    TRY(resolve_parent(fs, newpath, &dir, name));
    if (dir.root != in.root)
        return -EXDEV; /* hard links stay inside one subvolume */
    TRY(fs_root_writable(fs, dir.root));
    if (dir_lookup(b, dir, name, &loc, &type) == 0)
        return -EEXIST;
    TRY(trans_start(b, &t, 5));
    BtrfsKey k = mkkey(in.ino, BTRFS_INODE_ITEM_KEY, 0);
    int r = dir_add_entry(&t, dir, name, &k, BTRFS_FT_REG_FILE, &index);
    if (!r)
        r = inode_ref_add(&t, in, dir.ino, name, index);
    if (!r) {
        ii.nlink++;
        touch(&ii, b->transid);
        r = inode_put(&t, in, &ii);
    }
    trans_end(&t);
    return r < 0 ? trans_abort(&t, r) : 0;
}

/* ------------------------------------------------------- unlink, rmdir */

int fs_unlink(Fs *fs, const char *path)
{
    Btrfs *b = fs->b;
    Inode dir;
    char name[FS_NAME_MAX + 1];
    BtrfsDiskKey loc;
    uint8_t type;
    BtrfsInodeItem ii;
    Trans t;
    uint64_t index;

    TRY(resolve_parent(fs, path, &dir, name));
    TRY(dir_lookup(b, dir, name, &loc, &type));
    if (loc.type == BTRFS_ROOT_ITEM_KEY)
        return -EISDIR;
    Inode in = { dir.root, loc.objectid };
    TRY(inode_get(b, in, &ii));
    if (is_dir(&ii))
        return -EISDIR;
    TRY(fs_root_writable(fs, dir.root));
    TRY(trans_start_fallback_global(b, &t, 5));
    int r = inode_ref_del(&t, in, dir.ino, name, &index);
    if (!r)
        r = dir_del_entry(&t, dir, name, index);
    if (!r) {
        ii.nlink--;
        touch(&ii, b->transid);
        if (ii.nlink > 0) {
            r = inode_put(&t, in, &ii);
        } else {
            /* The last name: evict the inode, its data and the item. */
            r = truncate_inode_items(&t, in, &ii, 0);
            BtrfsKey k = mkkey(in.ino, BTRFS_INODE_ITEM_KEY, 0);
            if (!r)
                r = btrfs_delete_key(&t, in.root, &k);
        }
    }
    trans_end(&t);
    return r < 0 ? trans_abort(&t, r) : 0;
}

int fs_rmdir(Fs *fs, const char *path)
{
    Btrfs *b = fs->b;
    Inode dir;
    char name[FS_NAME_MAX + 1];
    BtrfsDiskKey loc;
    uint8_t type;
    BtrfsInodeItem ii;
    Trans t;
    uint64_t index;

    TRY(resolve_parent(fs, path, &dir, name));
    TRY(dir_lookup(b, dir, name, &loc, &type));
    if (loc.type == BTRFS_ROOT_ITEM_KEY) {
        /* rmdir of an empty subvolume deletes it, as in btrfs. */
        Root *sub = subvol_entry_root(b, dir.root, loc.objectid);
        if (!sub)
            return -ENOENT;
        if (!dir_is_empty(b, (Inode){ sub, BTRFS_FIRST_FREE_OBJECTID }))
            return -ENOTEMPTY;
        return fs_subvol_delete(fs, path);
    }
    Inode in = { dir.root, loc.objectid };
    TRY(inode_get(b, in, &ii));
    if (!is_dir(&ii))
        return -ENOTDIR;
    if (ii.size > 0)
        return -ENOTEMPTY;
    TRY(fs_root_writable(fs, dir.root));
    TRY(trans_start_fallback_global(b, &t, 5));
    int r = inode_ref_del(&t, in, dir.ino, name, &index);
    if (!r)
        r = dir_del_entry(&t, dir, name, index);
    BtrfsKey k = mkkey(in.ino, BTRFS_INODE_ITEM_KEY, 0);
    if (!r)
        r = btrfs_delete_key(&t, in.root, &k);
    trans_end(&t);
    return r < 0 ? trans_abort(&t, r) : 0;
}

/* --------------------------------------------------------------- readdir */

int fs_readdir(Fs *fs, const char *path, fs_dirent_fn fn, void *arg)
{
    Btrfs *b = fs->b;
    Inode dir, parent;
    BtrfsInodeItem ii;
    FsStat st;

    TRY(resolve_path(fs, path, &dir));
    TRY(inode_get(b, dir, &ii));
    if (!is_dir(&ii))
        return -ENOTDIR;
    TRY(stat_inode(b, dir, &st));
    fn(arg, ".", &st);
    parent = dir;
    TRY(parent_of(fs, &parent));
    TRY(stat_inode(b, parent, &st));
    fn(arg, "..", &st);

    /* Collect the entries first: the callback may use the filesystem. */
    typedef struct {
        char name[FS_NAME_MAX + 1];
        BtrfsDiskKey loc;
    } Ent;
    Ent *ents = NULL;
    size_t n = 0, cap = 0;
    Path p;
    BtrfsKey k = mkkey(dir.ino, BTRFS_DIR_INDEX_KEY, 0), f;
    int r = btrfs_search_forward(b, dir.root, &k, &p);
    while (r == 0) {
        item_key(p.nodes[0], p.slots[0], &f);
        if (f.objectid != dir.ino || f.type != BTRFS_DIR_INDEX_KEY)
            break;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            Ent *ne = realloc(ents, cap * sizeof *ne);
            if (!ne) {
                free(ents);
                return -ENOMEM;
            }
            ents = ne;
        }
        BtrfsDirItem di;
        uint8_t *d = item_ptr(p.nodes[0], p.slots[0]);
        memcpy(&di, d, sizeof di);
        memcpy(ents[n].name, d + sizeof di, di.name_len);
        ents[n].name[di.name_len] = '\0';
        ents[n].loc = di.location;
        n++;
        r = btrfs_next_item(b, &p);
    }
    for (size_t i = 0; i < n && r >= 0; i++) {
        Inode in = { dir.root, ents[i].loc.objectid };
        if (ents[i].loc.type == BTRFS_ROOT_ITEM_KEY) {
            in.root = subvol_entry_root(b, dir.root, ents[i].loc.objectid);
            in.ino = BTRFS_FIRST_FREE_OBJECTID;
            if (!in.root)
                continue;
        }
        if (stat_inode(b, in, &st) == 0)
            fn(arg, ents[i].name, &st);
    }
    free(ents);
    return r < 0 ? r : 0;
}
