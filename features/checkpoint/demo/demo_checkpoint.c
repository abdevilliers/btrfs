/*
 * demo_checkpoint.c - checkpointing a filesystem, step by step.
 *
 * Files are made with the normal file operations. A checkpoint (a
 * read-only btrfs snapshot of the live subvolume) is taken, the files are
 * changed, the old versions are read from the checkpoint, and the live
 * filesystem is rolled back. Each step prints the tree blocks written and
 * the space used, and runs btrfs check.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkpoint.h"
#include "fs.h"
#include "mem_disk.h"

static Disk *disk;
static Btrfs *b;
static Fs *fs;
static BtrfsStats before;

static void must(int r, const char *what)
{
    if (r < 0) {
        fprintf(stderr, "%s failed: %s\n", what, strerror(-r));
        exit(1);
    }
}

static void step(const char *title)
{
    printf("\n==== %s ====\n", title);
    btrfs_stats(b, &before);
}

static void put(Fs *f, const char *path, const char *text)
{
    int r = fs_create(f, path);
    if (r < 0 && r != -EEXIST)
        must(r, "create");
    must(fs_truncate(f, path, 0), "truncate");
    must((int)fs_write(f, path, 0, text, strlen(text)), "write");
}

static void cat(Fs *f, const char *path)
{
    char buf[256];
    FsStat st;
    int64_t n = fs_read(f, path, 0, buf, sizeof buf - 1);
    if (n < 0) {
        printf("  %-20s (no such file)\n", path);
        return;
    }
    buf[n] = '\0';
    fs_stat(f, path, &st);
    printf("  %-20s subvol %-3llu inode %-4llu \"%s\"\n", path, (unsigned long long)st.subvol,
           (unsigned long long)st.ino, buf);
}

static void footer(void)
{
    BtrfsStats now;
    FsSpace sp;
    btrfs_commit(b);
    btrfs_stats(b, &now);
    fs_space(b, &sp);
    printf("tree blocks written: %llu   bytes in extents: %llu   btrfs check: %s\n",
           (unsigned long long)(now.tree_blocks_written - before.tree_blocks_written), (unsigned long long)sp.bytes_used,
           btrfs_check(disk, 0) ? "PROBLEMS" : "clean");
}

static void list(void *arg, const FsSubvolInfo *i)
{
    (void)arg;
    printf("  ID %-4llu parent %-3llu otransid %-4llu %s%s\n", (unsigned long long)i->id,
           (unsigned long long)i->parent_id, (unsigned long long)i->otransid, i->name, i->read_only ? "  (read-only)" : "");
}

int main(void)
{
    BtrfsMkfsOpts o = { 4096, "demo" };
    disk = mem_disk_create(16384);
    if (!disk || btrfs_mkfs(disk, &o) < 0 || btrfs_mount(disk, NULL, &b) < 0 || ckpt_setup(b) < 0)
        return 1;
    fs = fs_open(b);

    step("1. Make some files in the live subvolume /@");
    must(fs_mkdir(fs, "/docs"), "mkdir");
    put(fs, "/docs/report.txt", "v1: first draft");
    put(fs, "/docs/notes.txt", "meeting at 10");
    for (int i = 0; i < 200; i++) { /* enough files for a tree of several levels */
        char p[32];
        snprintf(p, sizeof p, "/docs/old-%03d.txt", i);
        put(fs, p, "archive");
    }
    cat(fs, "/docs/report.txt");
    cat(fs, "/docs/notes.txt");
    footer();

    step("2. Take checkpoint \"monday\" (btrfs subvolume snapshot -r)");
    must(ckpt_create(b, "monday"), "checkpoint");
    footer();
    printf("-> of the subvolume's tree, the snapshot copied ONE block: its root. The root's\n"
           "   children each gained a reference (TREE_BLOCK_REF from the new subvolume).\n"
           "   The other blocks written are the commit's own: root, extent, free space trees.\n");

    step("3. Update report.txt, delete notes.txt, create todo.txt");
    put(fs, "/docs/report.txt", "v2: FINAL version");
    must(fs_unlink(fs, "/docs/notes.txt"), "unlink");
    put(fs, "/docs/todo.txt", "buy milk");
    cat(fs, "/docs/report.txt");
    cat(fs, "/docs/notes.txt");
    cat(fs, "/docs/todo.txt");
    footer();
    printf("-> only the blocks on the changed paths were copied; monday shares the rest.\n");

    step("4. Look inside \"monday\" (read-only)");
    Fs *old = ckpt_open_fs(b, "monday");
    cat(old, "/docs/report.txt");
    cat(old, "/docs/notes.txt");
    cat(old, "/docs/todo.txt");
    printf("  writing to it: %s\n", strerror(-fs_create(old, "/docs/x")));
    fs_close(old);

    step("5. Roll back to \"monday\" (like snapper rollback)");
    fs_close(fs);
    must(ckpt_rollback(b, "monday"), "rollback");
    fs = fs_open(b);
    cat(fs, "/docs/report.txt");
    cat(fs, "/docs/notes.txt");
    cat(fs, "/docs/todo.txt");
    footer();
    printf("-> a writable snapshot of monday is now the default subvolume; the old live\n"
           "   subvolume was deleted, and the cleaner freed what only it used.\n");
    printf("subvolumes (btrfs subvolume list):\n");
    fs_subvol_list(b, list, NULL);

    step("6. Delete \"monday\"");
    must(ckpt_delete(b, "monday"), "delete");
    footer();
    btrfs_commit(b);

    fs_close(fs);
    btrfs_unmount(b);
    disk_close(disk);
    return 0;
}
