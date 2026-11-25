/*
 * test_subvol.c - subvolumes and snapshots: create, snapshot (one block
 * copied), copy-on-write keeping them apart, read-only snapshots, the
 * default subvolume, the rules on deleting, the cleaner freeing a
 * deleted snapshot's space, and many files in many snapshots against a
 * model (tall shared trees, FULL_BACKREF, drop_snapshot).
 */
#include <errno.h>

#include "test_util.h"

static int count;
static void count_subvol(void *arg, const FsSubvolInfo *info)
{
    (void)arg;
    (void)info;
    count++;
}

static void test_snapshots(void)
{
    Disk *d = new_disk(16384);
    Btrfs *b = mount_ok(d);
    Fs *fs = fs_open(b);
    FsStat st;
    FsSubvolInfo info;
    BtrfsStats before, after;
    uint64_t id, snap;

    CHECK(fs_subvol_create(fs, "/home") == 0);
    CHECK(fs_subvol_lookup(fs, "/home", &id) == 0 && id == 256);
    CHECK(fs_subvol_create(fs, "/home") == -EEXIST);
    CHECK(put(fs, "/home/a.txt", "version 1") == 0);
    CHECK(fs_stat(fs, "/home/a.txt", &st) == 0 && st.subvol == 256 && st.ino == 257);
    CHECK(fs_mkdir(fs, "/snaps") == 0);
    commit_and_check(b);

    btrfs_stats(b, &before);
    CHECK(fs_subvol_snapshot(fs, "/home", "/snaps/monday", 1, &snap) == 0); /* -r */
    btrfs_stats(b, &after);
    CHECK(snap == 257);
    CHECK(after.commits == before.commits + 1); /* a snapshot is made by a commit */
    CHECK(fs_subvol_info(b, snap, &info) == 0 && info.read_only && info.parent_id == 5);
    FsSubvolInfo src;
    CHECK(fs_subvol_info(b, 256, &src) == 0);
    CHECK(fs_subvol_info(b, snap, &info) == 0 && memcmp(info.parent_uuid, src.uuid, 16) == 0);
    CHECK(btrfs_check(d, 1) == 0);

    /* Copy-on-write keeps them apart. */
    CHECK(put(fs, "/home/a.txt", "version 2") == 0);
    CHECK(put(fs, "/home/b.txt", "new") == 0);
    CHECK(file_is(fs, "/home/a.txt", "version 2"));
    CHECK(file_is(fs, "/snaps/monday/a.txt", "version 1"));
    CHECK(file_is(fs, "/snaps/monday/b.txt", NULL));
    CHECK(fs_create(fs, "/snaps/monday/x") == -EROFS); /* read-only */
    CHECK(fs_subvol_snapshot(fs, "/snaps", "/x", 0, NULL) == -EINVAL); /* not a subvolume */
    commit_and_check(b);

    /* A writable snapshot of the read-only one, made the default. */
    CHECK(fs_subvol_snapshot(fs, "/snaps/monday", "/restored", 0, &id) == 0);
    CHECK(fs_set_default_subvol(b, id) == 0 && fs_get_default_subvol(b) == id);
    Fs *def = fs_open(b);
    CHECK(def && fs_subvol_id(def) == id);
    CHECK(file_is(def, "/a.txt", "version 1"));
    CHECK(put(def, "/a.txt", "changed in restored") == 0);
    CHECK(file_is(fs, "/snaps/monday/a.txt", "version 1"));
    commit_and_check(b);

    count = 0;
    CHECK(fs_subvol_list(b, count_subvol, NULL) == 0 && count == 3);
    CHECK(fs_subvol_delete(fs, "/restored") == -EPERM); /* the default */
    CHECK(fs_subvol_delete(fs, "/snaps") == -EINVAL);   /* not a subvolume */
    CHECK(fs_subvol_create(fs, "/home/inner") == 0);
    CHECK(fs_subvol_delete(fs, "/home") == -ENOTEMPTY); /* holds a subvolume */
    CHECK(fs_rmdir(fs, "/home/inner") == 0);            /* rmdir of an empty subvolume */
    CHECK(fs_set_default_subvol(b, 5) == 0);
    fs_close(def);
    CHECK(fs_subvol_delete(fs, "/restored") == 0);
    CHECK(fs_stat(fs, "/restored", &st) == -ENOENT);
    commit_and_check(b);

    /* Snapshot of a subvolume inside itself. */
    CHECK(fs_subvol_snapshot(fs, "/home", "/home/self", 0, &id) == 0);
    CHECK(file_is(fs, "/home/self/a.txt", "version 2"));
    CHECK(fs_stat(fs, "/home/self/self", &st) == -ENOENT); /* it is not inside itself */
    commit_and_check(b);

    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    b = mount_ok(d);
    fs = fs_open(b);
    CHECK(file_is(fs, "/snaps/monday/a.txt", "version 1"));
    CHECK(file_is(fs, "/home/a.txt", "version 2"));
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    CHECK(btrfs_check(d, 1) == 0);
    disk_close(d);
    printf("ok   subvolumes, read-only and writable snapshots, default subvolume, delete rules\n");
}

/* A deleted snapshot's space is freed by the cleaner after the commit. */
static void test_delete_frees_space(void)
{
    Disk *d = new_disk(16384);
    Btrfs *b = mount_ok(d);
    Fs *fs = fs_open(b);
    FsSpace s0, s1, s2;
    static char data[100000];
    char p[32];

    memset(data, 'k', sizeof data);
    CHECK(fs_subvol_create(fs, "/vol") == 0);
    for (int i = 0; i < 20; i++) {
        snprintf(p, sizeof p, "/vol/f%d", i);
        CHECK(fs_create(fs, p) == 0 && fs_write(fs, p, 0, data, sizeof data) == (int64_t)sizeof data);
    }
    commit_and_check(b);
    CHECK(fs_space(b, &s0) == 0);

    CHECK(fs_subvol_snapshot(fs, "/vol", "/snap", 0, NULL) == 0);
    CHECK(fs_space(b, &s1) == 0);
    CHECK(s1.bytes_used <= s0.bytes_used + 4 * 4096); /* a snapshot costs a block or two */

    /* Rewrite everything in /vol: the snapshot now holds the old data. */
    for (int i = 0; i < 20; i++) {
        snprintf(p, sizeof p, "/vol/f%d", i);
        CHECK(fs_write(fs, p, 0, data, sizeof data) == (int64_t)sizeof data);
    }
    commit_and_check(b);
    CHECK(fs_space(b, &s1) == 0);
    CHECK(s1.data_used >= s0.data_used + 20 * sizeof data);

    CHECK(fs_subvol_delete(fs, "/snap") == 0);
    commit_and_check(b); /* the commit; the cleaner then drops the tree */
    commit_and_check(b); /* ... and this commit makes that durable    */
    CHECK(fs_space(b, &s2) == 0);
    CHECK(s2.data_used == s0.data_used);
    CHECK(s2.bytes_used <= s0.bytes_used + 8 * 4096);
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    disk_close(d);
    printf("ok   deleting a snapshot frees exactly the data only it used (after the cleaner)\n");
}

/* Thousands of files, several snapshots, writes into the snapshots too. */
#define NF 3000
#define NS 4
typedef char Txt[16];
static Txt live[NF], snap[NS][NF];
static int has[NS];

static void path_of(char *p, size_t n, const char *pre, int i)
{
    snprintf(p, n, "%s/d%d/a-file-with-a-long-name-%d", pre, i % 30, i);
}

static void expect(Fs *fs, const char *pre, Txt *m, int i)
{
    char p[128];
    path_of(p, sizeof p, pre, i);
    CHECK(file_is(fs, p, m[i][0] ? m[i] : NULL));
}

static void test_many_snapshots(void)
{
    Disk *d = new_disk(32768);
    Btrfs *b = mount_ok(d);
    Fs *fs = fs_open(b);
    char p[128], sp[32];
    int checks = 0;

    CHECK(fs_mkdir(fs, "/snaps") == 0);
    for (int i = 0; i < 30; i++) {
        snprintf(p, sizeof p, "/d%d", i);
        CHECK(fs_mkdir(fs, p) == 0);
    }
    srand(9);
    for (int step = 0; step < 25000; step++) {
        int op = rand() % 100, i = rand() % NF, k = rand() % NS;
        snprintf(sp, sizeof sp, "/snaps/s%d", k);
        if (op < 55) {
            path_of(p, sizeof p, "", i);
            snprintf(live[i], sizeof live[i], "v%d", step);
            CHECK(put(fs, p, live[i]) == 0);
        } else if (op < 70) {
            path_of(p, sizeof p, "", i);
            CHECK(fs_unlink(fs, p) == (live[i][0] ? 0 : -ENOENT));
            live[i][0] = 0;
        } else if (op < 80) {
            expect(fs, "", live, i);
            if (has[k])
                expect(fs, sp, snap[k], i);
        } else if (op < 82) {
            if (has[k]) {
                CHECK(fs_subvol_delete(fs, sp) == 0);
                has[k] = 0;
            } else {
                CHECK(fs_subvol_snapshot(fs, "/", sp, 0, NULL) == 0);
                memcpy(snap[k], live, sizeof live);
                has[k] = 1;
            }
        } else if (op < 86 && has[k]) {
            char q[128];
            path_of(q, sizeof q, sp, i);
            snprintf(snap[k][i], sizeof snap[k][i], "S%d", step);
            CHECK(put(fs, q, snap[k][i]) == 0);
        } else if (op < 87) {
            CHECK(btrfs_commit(b) == 0);
            if (rand() % 3 == 0) {
                CHECK(btrfs_check(d, 1) == 0);
                checks++;
            }
        }
    }
    commit_and_check(b);
    for (int i = 0; i < NF; i++) {
        expect(fs, "", live, i);
        for (int k = 0; k < NS; k++)
            if (has[k]) {
                snprintf(sp, sizeof sp, "/snaps/s%d", k);
                expect(fs, sp, snap[k], i);
            }
    }
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    CHECK(btrfs_check(d, 1) == 0);
    disk_close(d);
    printf("ok   %d files across %d snapshots, 25000 operations, %d checks along the way\n", NF, NS, checks + 2);
}

int main(void)
{
    test_snapshots();
    test_delete_frees_space();
    test_many_snapshots();
    printf("subvol: all tests passed\n");
    return 0;
}
