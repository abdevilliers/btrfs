/*
 * test_crash.c - crash safety.
 *
 *   - Power cut at EVERY write of a commit: the disk always mounts as
 *     either the previous commit or the new one, never a mix, and btrfs
 *     check is clean.
 *   - A damaged primary superblock: mount refuses (-EIO, as the kernel
 *     reads only the primary); btrfs_super_recover copies the mirror back.
 *   - A damaged root tree: -o usebackuproot mounts the previous commit
 *     from the superblock's backup roots.
 */
#include <errno.h>

#include "fault_disk.h"
#include "test_util.h"

#define BLOCKS 20000 /* 78 MiB: room for the superblock copy at 64 MiB */

/* The base state, committed: generation G. */
static void make_base(Disk *d)
{
    BtrfsMkfsOpts o = { 4096, NULL };
    CHECK(btrfs_mkfs(d, &o) == 0);
    Btrfs *b = mount_ok(d);
    Fs *fs = fs_open(b);
    char p[32];
    CHECK(fs_mkdir(fs, "/d") == 0);
    for (int i = 0; i < 40; i++) {
        snprintf(p, sizeof p, "/d/old%d", i);
        CHECK(put(fs, p, "old") == 0);
    }
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
}

/* Changes to commit: generation G + 1. */
static void make_changes(Fs *fs)
{
    static char big[50000];
    char p[32];
    memset(big, 'n', sizeof big);
    for (int i = 0; i < 40; i += 2) {
        snprintf(p, sizeof p, "/d/old%d", i);
        CHECK(fs_unlink(fs, p) == 0);
    }
    for (int i = 0; i < 30; i++) {
        snprintf(p, sizeof p, "/d/new%d", i);
        CHECK(put(fs, p, "new") == 0);
    }
    CHECK(fs_create(fs, "/big") == 0);
    CHECK(fs_write(fs, "/big", 0, big, sizeof big) == (int64_t)sizeof big);
}

/* 0 = exactly the old state, 1 = exactly the new one, -1 = neither. */
static int which_state(Fs *fs)
{
    int old = file_is(fs, "/d/old0", "old") && file_is(fs, "/d/new0", NULL) && file_is(fs, "/big", NULL);
    int new = file_is(fs, "/d/old0", NULL) && file_is(fs, "/d/new29", "new") && file_is(fs, "/d/old1", "old");
    if (old) {
        for (int i = 0; i < 40; i++) {
            char p[32];
            snprintf(p, sizeof p, "/d/old%d", i);
            if (!file_is(fs, p, "old"))
                return -1;
        }
        return 0;
    }
    if (new) {
        char buf[60000];
        if (fs_read(fs, "/big", 0, buf, sizeof buf) != 50000)
            return -1;
        for (int i = 0; i < 50000; i++)
            if (buf[i] != 'n')
                return -1;
        return 1;
    }
    return -1;
}

static void test_power_cut_everywhere(void)
{
    /* First, how many writes does the commit make? */
    Disk *d = mem_disk_create(BLOCKS);
    make_base(d);
    Btrfs *b = mount_ok(d);
    Fs *fs = fs_open(b);
    make_changes(fs);
    uint64_t before = disk_write_count(d);
    CHECK(btrfs_commit(b) == 0);
    uint64_t total = disk_write_count(d) - before;
    fs_close(fs);
    btrfs_crash(b);
    disk_close(d);

    int olds = 0, news = 0;
    for (uint64_t cut = 0; cut <= total; cut++) {
        Disk *inner = mem_disk_create(BLOCKS);
        make_base(inner);
        Disk *fd = fault_disk_wrap(inner);
        b = mount_ok(fd);
        fs = fs_open(b);
        make_changes(fs);
        fault_disk_cut_after(fd, cut); /* the power goes after `cut` writes */
        btrfs_commit(b);
        fs_close(fs);
        btrfs_crash(b);
        disk_close(fd);

        CHECK(btrfs_check(inner, 1) == 0);
        b = mount_ok(inner);
        fs = fs_open(b);
        int s = which_state(fs);
        CHECK(s >= 0);
        if (s)
            news++;
        else
            olds++;
        CHECK(s == (cut >= total - 1)); /* new once the primary superblock is written */
        fs_close(fs);
        CHECK(btrfs_unmount(b) == 0);
        disk_close(inner);
    }
    printf("ok   power cut after each of the %llu writes of a commit: old state %d times, new %d, never a mix\n",
           (unsigned long long)total, olds, news);
}

static void test_super_recover(void)
{
    Disk *d = mem_disk_create(BLOCKS);
    make_base(d);
    uint64_t gen = btrfs_disk_generation(d);
    uint8_t junk[BLOCK_SIZE];
    Btrfs *b;

    memset(junk, 0x5a, sizeof junk);
    CHECK(disk_write(d, 16, junk) == 0); /* the primary superblock, at 64 KiB */
    CHECK(btrfs_mount(d, NULL, &b) == -ENOENT);  /* no magic left: "not btrfs" */
    CHECK(btrfs_disk_generation(d) == 0);
    CHECK(btrfs_super_recover(d) == 1);          /* from the copy at 64 MiB */
    CHECK(btrfs_disk_generation(d) == gen);
    CHECK(btrfs_super_recover(d) == 0);          /* nothing more to do */
    b = mount_ok(d);
    Fs *fs = fs_open(b);
    CHECK(file_is(fs, "/d/old7", "old"));
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    CHECK(btrfs_check(d, 1) == 0);

    /* A small disk has only the primary: nothing to recover from. */
    Disk *small = new_disk(4096);
    CHECK(disk_write(small, 16, junk) == 0);
    CHECK(btrfs_super_recover(small) == -ENOENT);
    disk_close(small);
    disk_close(d);
    printf("ok   damaged primary superblock: super-recover restores it from the mirror\n");
}

static void test_backup_roots(void)
{
    Disk *d = mem_disk_create(BLOCKS);
    make_base(d);
    Btrfs *b = mount_ok(d);
    Fs *fs = fs_open(b);
    BtrfsMountOpts backup = { 1, 0 };
    uint64_t gen1 = btrfs_disk_generation(d);

    CHECK(put(fs, "/later.txt", "after the base") == 0);
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    uint64_t gen2 = btrfs_disk_generation(d);
    CHECK(gen2 == gen1 + 1);

    /* Damage the root tree block the superblock points at. */
    BtrfsSuper sb;
    CHECK(disk_read(d, 16, &sb) == 0);
    uint8_t sec[BLOCK_SIZE];
    CHECK(disk_read(d, (uint32_t)(sb.root / BLOCK_SIZE), sec) == 0);
    sec[500] ^= 1;
    CHECK(disk_write(d, (uint32_t)(sb.root / BLOCK_SIZE), sec) == 0);

    CHECK(btrfs_mount(d, NULL, &b) == -EIO);
    CHECK(btrfs_mount(d, &backup, &b) == 0);     /* -o usebackuproot */
    CHECK(btrfs_generation(b) == gen1);           /* the commit before */
    fs = fs_open(b);
    CHECK(file_is(fs, "/later.txt", NULL));
    CHECK(file_is(fs, "/d/old3", "old"));
    CHECK(put(fs, "/after-recovery", "ok") == 0);
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);                 /* a new commit over it */
    CHECK(btrfs_check(d, 1) == 0);
    b = mount_ok(d);
    fs = fs_open(b);
    CHECK(file_is(fs, "/after-recovery", "ok"));
    fs_close(fs);
    CHECK(btrfs_unmount(b) == 0);
    disk_close(d);
    printf("ok   damaged root tree: -o usebackuproot mounts the previous commit\n");
}

int main(void)
{
    test_power_cut_everywhere();
    test_super_recover();
    test_backup_roots();
    printf("crash: all tests passed\n");
    return 0;
}
