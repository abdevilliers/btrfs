/*
 * test_fs.c - files and directories: create, write, read, links, inline
 * and regular extents, truncate, holes, copy-on-write of data, checksums,
 * a full disk, and random operations against a model, with btrfs check
 * after every commit.
 */
#include <errno.h>

#include "test_util.h"

static int count;
static void count_entry(void *arg, const char *name, const FsStat *st)
{
    (void)arg;
    (void)name;
    (void)st;
    count++;
}

typedef struct {
    int n;
    FsExtent e[64];
} Extents;

static void collect(void *arg, const FsExtent *e)
{
    Extents *x = arg;
    if (x->n < 64)
        x->e[x->n++] = *e;
}

static void test_basics(void)
{
    Disk *d = new_disk(8192);
    Btrfs *b = mount_ok(d);
    Fs *fs = fs_open(b);
    FsStat st;

    CHECK(fs != NULL && fs_subvol_id(fs) == BTRFS_FS_TREE_OBJECTID);
    CHECK(fs_stat(fs, "/", &st) == 0 && st.type == FS_DIR && st.ino == FS_ROOT_INO && st.nlink == 1);
    CHECK(fs_mkdir(fs, "/docs") == 0);
    CHECK(fs_create(fs, "/docs/notes.txt") == 0);
    CHECK(fs_write(fs, "/docs/notes.txt", 0, "hello", 5) == 5);
    CHECK(file_is(fs, "/docs/notes.txt", "hello"));
    CHECK(fs_write(fs, "/docs/notes.txt", 5, ", world", 7) == 7);
    CHECK(fs_write(fs, "/docs/notes.txt", 0, "J", 1) == 1);
    CHECK(file_is(fs, "/docs/notes.txt", "Jello, world"));
    CHECK(fs_stat(fs, "/docs/notes.txt", &st) == 0 && st.type == FS_FILE && st.size == 12 && st.nlink == 1);
    CHECK(st.ino == 258); /* "/" is 256, /docs 257: highest + 1 */
    CHECK(fs_stat(fs, "/docs", &st) == 0 && st.size == 2 * strlen("notes.txt") && st.nlink == 1);
    CHECK(fs_stat(fs, "//docs//./notes.txt", &st) == 0);
    CHECK(fs_stat(fs, "/docs/../docs/notes.txt", &st) == 0);

    count = 0;
    CHECK(fs_readdir(fs, "/docs", count_entry, NULL) == 0 && count == 3);
    CHECK(fs_create(fs, "/docs/notes.txt") == -EEXIST);
    CHECK(fs_create(fs, "/missing/x") == -ENOENT);
    CHECK(fs_create(fs, "/docs/notes.txt/x") == -ENOTDIR);
    CHECK(fs_create(fs, "relative") == -EINVAL);
    CHECK(fs_create(fs, "/") == -EINVAL);
    char longname[300];
    memset(longname, 'a', sizeof longname);
    longname[0] = '/';
    longname[299] = '\0';
    CHECK(fs_create(fs, longname) == -ENAMETOOLONG);
    CHECK(fs_read(fs, "/docs", 0, (char[8]){ 0 }, 8) == -EISDIR);
    CHECK(fs_unlink(fs, "/docs") == -EISDIR);
    CHECK(fs_rmdir(fs, "/docs") == -ENOTEMPTY);
    CHECK(fs_rmdir(fs, "/docs/notes.txt") == -ENOTDIR);
    commit_and_check(b);

    CHECK(fs_unlink(fs, "/docs/notes.txt") == 0);
    CHECK(fs_stat(fs, "/docs/notes.txt", &st) == -ENOENT);
    CHECK(fs_rmdir(fs, "/docs") == 0);
    commit_and_check(b);
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    disk_close(d);
    printf("ok   files and directories: create, write, read, stat, readdir, unlink, rmdir\n");
}

static void test_links_and_names(void)
{
    Disk *d = new_disk(8192);
    Btrfs *b = mount_ok(d);
    Fs *fs = fs_open(b);
    FsStat st;

    CHECK(put(fs, "/orig", "shared text") == 0);
    CHECK(fs_mkdir(fs, "/dir") == 0);
    CHECK(fs_link(fs, "/orig", "/dir/alias") == 0);
    CHECK(fs_link(fs, "/orig", "/alias2") == 0); /* two names in "/": one INODE_REF item, two entries */
    CHECK(fs_stat(fs, "/orig", &st) == 0 && st.nlink == 3);
    CHECK(fs_write(fs, "/dir/alias", 0, "S", 1) == 1);
    CHECK(file_is(fs, "/orig", "Shared text"));
    CHECK(fs_link(fs, "/dir", "/dir2") == -EPERM);
    commit_and_check(b);
    CHECK(fs_unlink(fs, "/orig") == 0);
    CHECK(fs_unlink(fs, "/alias2") == 0);
    CHECK(file_is(fs, "/dir/alias", "Shared text"));
    CHECK(fs_stat(fs, "/dir/alias", &st) == 0 && st.nlink == 1);
    commit_and_check(b);

    /* Many names in one directory: the directory's items split across
     * leaves; every name stays findable. */
    char p[64];
    for (int i = 0; i < 600; i++) {
        snprintf(p, sizeof p, "/dir/entry-%04d", i);
        CHECK(fs_create(fs, p) == 0);
    }
    commit_and_check(b);
    count = 0;
    CHECK(fs_readdir(fs, "/dir", count_entry, NULL) == 0 && count == 603);
    for (int i = 0; i < 600; i += 2) {
        snprintf(p, sizeof p, "/dir/entry-%04d", i);
        CHECK(fs_unlink(fs, p) == 0);
    }
    for (int i = 1; i < 600; i += 2) {
        snprintf(p, sizeof p, "/dir/entry-%04d", i);
        CHECK(fs_stat(fs, p, &st) == 0);
    }
    commit_and_check(b);
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    disk_close(d);
    printf("ok   hard links, two names in one directory, 600 entries in one directory\n");
}

static void test_extents(void)
{
    Disk *d = new_disk(8192);
    Btrfs *b = mount_ok(d);
    Fs *fs = fs_open(b);
    FsStat st;
    Extents x;
    static char big[300000], back[300000];

    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (char)('a' + i % 26);

    /* Up to 2048 bytes: inline, in the FS tree. */
    CHECK(fs_create(fs, "/small") == 0);
    CHECK(fs_write(fs, "/small", 0, big, 2000) == 2000);
    x.n = 0;
    CHECK(fs_fiemap(fs, "/small", collect, &x) == 0);
    CHECK(x.n == 1 && (x.e[0].flags & FIEMAP_EXTENT_INLINE) && x.e[0].length == 2000);
    CHECK(fs_stat(fs, "/small", &st) == 0 && st.nbytes == 2000);
    /* Past 2048 it becomes a regular extent. */
    CHECK(fs_write(fs, "/small", 2000, big + 2000, 100) == 100);
    x.n = 0;
    CHECK(fs_fiemap(fs, "/small", collect, &x) == 0);
    CHECK(x.n == 1 && !(x.e[0].flags & FIEMAP_EXTENT_INLINE) && x.e[0].physical != 0);
    CHECK(fs_read(fs, "/small", 0, back, 3000) == 2100 && memcmp(back, big, 2100) == 0);

    /* A big file; an overwrite in the middle goes to a NEW extent and
     * splits the old item in two (__btrfs_drop_extents). */
    CHECK(fs_create(fs, "/big") == 0);
    CHECK(fs_write(fs, "/big", 0, big, sizeof big) == (int64_t)sizeof big);
    x.n = 0;
    CHECK(fs_fiemap(fs, "/big", collect, &x) == 0);
    uint64_t first = x.e[0].physical;
    commit_and_check(b);
    CHECK(fs_write(fs, "/big", 8192, "XXXX", 4) == 4);
    big[8192] = big[8193] = big[8194] = big[8195] = 'X';
    x.n = 0;
    CHECK(fs_fiemap(fs, "/big", collect, &x) == 0);
    CHECK(x.n == 3 && x.e[0].physical == first && x.e[0].length == 8192);
    CHECK(x.e[1].logical == 8192 && x.e[1].length == 4096 && x.e[1].physical != first + 8192);
    CHECK(x.e[2].physical == first + 12288); /* the tail still points into the old extent */
    CHECK(fs_read(fs, "/big", 0, back, sizeof back) == (int64_t)sizeof big && memcmp(back, big, sizeof big) == 0);
    commit_and_check(b);

    /* Truncate down, then grow: the grown part reads as zeros. */
    CHECK(fs_truncate(fs, "/big", 5000) == 0);
    CHECK(fs_truncate(fs, "/big", 20000) == 0);
    CHECK(fs_read(fs, "/big", 0, back, 20000) == 20000 && memcmp(back, big, 5000) == 0);
    for (int i = 5000; i < 20000; i++)
        CHECK(back[i] == 0);
    CHECK(fs_stat(fs, "/big", &st) == 0 && st.size == 20000 && st.nbytes == 8192);

    /* A hole: nothing is stored for it (NO_HOLES). */
    CHECK(fs_create(fs, "/sparse") == 0);
    CHECK(fs_write(fs, "/sparse", 1000000, "end", 3) == 3);
    CHECK(fs_stat(fs, "/sparse", &st) == 0 && st.size == 1000003 && st.nbytes == 4096);
    CHECK(fs_read(fs, "/sparse", 999990, back, 100) == 13 && back[0] == 0 && memcmp(back + 10, "end", 3) == 0);
    commit_and_check(b);
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);

    /* Remount: the same data, from the disk. */
    b = mount_ok(d);
    fs = fs_open(b);
    CHECK(fs_read(fs, "/small", 0, back, 3000) == 2100 && memcmp(back, big, 2100) == 0);
    CHECK(fs_read(fs, "/big", 0, back, 5000) == 5000 && memcmp(back, big, 5000) == 0);
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    disk_close(d);
    printf("ok   extents: inline up to 2048 bytes, copy-on-write overwrite splits items, truncate, holes\n");
}

static void test_checksums(void)
{
    Disk *d = new_disk(8192);
    Btrfs *b = mount_ok(d);
    Fs *fs = fs_open(b);
    Extents x = { 0 };
    char buf[10000];
    uint8_t sec[BLOCK_SIZE];

    memset(buf, 'q', sizeof buf);
    CHECK(fs_create(fs, "/f") == 0);
    CHECK(fs_write(fs, "/f", 0, buf, sizeof buf) == (int64_t)sizeof buf);
    CHECK(fs_fiemap(fs, "/f", collect, &x) == 0 && x.n == 1);
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);

    /* Damage one byte of the data on the disk. */
    uint32_t s = (uint32_t)(x.e[0].physical / BLOCK_SIZE) + 1;
    CHECK(disk_read(d, s, sec) == 0);
    sec[7] ^= 0xff;
    CHECK(disk_write(d, s, sec) == 0);

    CHECK(btrfs_check(d, 0) == 1); /* the scrub finds it */
    b = mount_ok(d);
    fs = fs_open(b);
    CHECK(fs_read(fs, "/f", 0, buf, 4096) == 4096);       /* sector 0 is fine */
    CHECK(fs_read(fs, "/f", 4096, buf, 10) == -EIO);       /* sector 1 is not  */
    fs_close(fs);
    btrfs_crash(b);
    disk_close(d);
    printf("ok   data checksums: a damaged sector reads as -EIO, btrfs check reports it\n");
}

static void test_full_disk(void)
{
    Disk *d = new_disk(2048); /* 8 MiB */
    Btrfs *b = mount_ok(d);
    Fs *fs = fs_open(b);
    static char data[200000];
    char p[32];
    int files = 0;
    int64_t r = 0;

    memset(data, 'z', sizeof data);
    for (int i = 0; r >= 0; i++) {
        snprintf(p, sizeof p, "/f%d", i);
        int c = fs_create(fs, p);
        if (c < 0) {
            r = c;
            break;
        }
        r = fs_write(fs, p, 0, data, sizeof data);
        if (r >= 0)
            files++;
    }
    CHECK(r == -ENOSPC);
    CHECK(!btrfs_is_read_only(b)); /* refused before changing anything: no abort */
    commit_and_check(b);
    /* Deleting works on a full disk, and the space comes back. */
    for (int i = 0; i < files; i += 2) {
        snprintf(p, sizeof p, "/f%d", i);
        CHECK(fs_unlink(fs, p) == 0);
    }
    commit_and_check(b);
    CHECK(fs_create(fs, "/again") == 0);
    CHECK(fs_write(fs, "/again", 0, data, sizeof data) == (int64_t)sizeof data);
    commit_and_check(b);
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    disk_close(d);
    printf("ok   a full disk refuses with -ENOSPC and stays consistent; deleting frees space (%d files fit)\n",
           files);
}

/* Random operations on a few files, against a model. */
#define NF 10
#define MAXF 70000
typedef struct {
    int exists;
    uint64_t len;
    unsigned char data[MAXF];
} ModelFile;

static void test_random(void)
{
    static ModelFile live[NF], committed[NF];
    static unsigned char buf[MAXF + 16];
    Disk *d = new_disk(16384);
    Btrfs *b = mount_ok(d);
    Fs *fs = fs_open(b);
    char p[32], q[32];
    int checks = 0;

    CHECK(fs_mkdir(fs, "/d") == 0);
    commit_and_check(b);
    memcpy(committed, live, sizeof live);
    srand(5);
    for (int step = 0; step < 6000; step++) {
        int i = rand() % NF, j = rand() % NF, op = rand() % 20;
        snprintf(p, sizeof p, i % 2 ? "/d/f%d" : "/f%d", i);
        snprintf(q, sizeof q, j % 2 ? "/d/f%d" : "/f%d", j);
        ModelFile *m = &live[i];
        if (op < 2) {
            CHECK(fs_create(fs, p) == (m->exists ? -EEXIST : 0));
            if (!m->exists) {
                m->exists = 1;
                m->len = 0;
            }
        } else if (op < 9) {
            uint64_t off = (uint64_t)(rand() % 2 ? rand() % 3000 : rand() % 60000);
            uint64_t n = (uint64_t)(rand() % 2 ? rand() % 200 + 1 : rand() % 9000 + 1);
            for (uint64_t k = 0; k < n; k++)
                buf[k] = (unsigned char)rand();
            int64_t r = fs_write(fs, p, off, buf, n);
            if (!m->exists) {
                CHECK(r == -ENOENT);
            } else {
                CHECK(r == (int64_t)n);
                if (off > m->len)
                    memset(m->data + m->len, 0, off - m->len);
                memcpy(m->data + off, buf, n);
                if (off + n > m->len)
                    m->len = off + n;
            }
        } else if (op < 11) {
            uint64_t size = (uint64_t)(rand() % 2 ? rand() % 3000 : rand() % 65000);
            CHECK(fs_truncate(fs, p, size) == (m->exists ? 0 : -ENOENT));
            if (m->exists) {
                if (size > m->len)
                    memset(m->data + m->len, 0, size - m->len);
                m->len = size;
            }
        } else if (op < 12) {
            CHECK(fs_unlink(fs, p) == (m->exists ? 0 : -ENOENT));
            m->exists = 0;
        } else if (op < 14) {
            /* Reflink the whole file (the duplication feature's core call). */
            ModelFile *g = &live[j];
            if (!m->exists || i == j)
                continue;
            if (!g->exists) {
                CHECK(fs_create(fs, q) == 0);
                g->exists = 1;
            }
            CHECK(fs_truncate(fs, q, 0) == 0);
            CHECK(fs_clone_range(fs, p, 0, q, 0, 0) == 0);
            g->len = m->len;
            memcpy(g->data, m->data, m->len);
        } else if (op < 17) {
            int64_t r = fs_read(fs, p, 0, buf, MAXF + 16);
            if (!m->exists)
                CHECK(r == -ENOENT);
            else
                CHECK(r == (int64_t)m->len && memcmp(buf, m->data, m->len) == 0);
        } else if (op < 19) {
            commit_and_check(b);
            checks++;
            memcpy(committed, live, sizeof live);
        } else {
            /* A crash: back to the last commit. */
            fs_close(fs);
            btrfs_crash(b);
            CHECK(btrfs_check(d, 1) == 0);
            b = mount_ok(d);
            fs = fs_open(b);
            memcpy(live, committed, sizeof live);
        }
    }
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    CHECK(btrfs_check(d, 1) == 0);
    disk_close(d);
    printf("ok   6000 random writes, truncates, unlinks, reflinks and crashes; %d commits, each checked\n", checks);
}

int main(void)
{
    test_basics();
    test_links_and_names();
    test_extents();
    test_checksums();
    test_full_disk();
    test_random();
    printf("fs: all tests passed\n");
    return 0;
}
