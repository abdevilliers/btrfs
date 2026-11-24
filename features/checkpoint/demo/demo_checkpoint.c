/*
 * demo_checkpoint.c - checkpointing a filesystem, step by step.
 *
 * Files are created, written and updated with the normal file operations
 * (fs_create, fs_write, fs_read, fs_unlink). A checkpoint is then taken
 * of the whole volume, which holds the filesystem, and the demo shows:
 * taking it writes nothing; updating a file afterwards copies only what
 * changes; the old version can still be read; rollback brings back every
 * file, directory and inode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "checkpoint.h"
#include "fs.h"
#include "mem_disk.h"
#include "volume.h"

static Disk *disk;
static Volume *vol;
static Fs *fs;
static uint64_t writes_before;

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
    writes_before = disk_write_count(disk);
}

static void put(const char *path, const char *text)
{
    int r = fs_create(fs, path);
    if (r < 0 && r != -17 /* EEXIST */)
        must(r, "create");
    must(fs_truncate(fs, path, 0), "truncate");
    must(fs_write(fs, path, 0, text, (uint32_t)strlen(text)), "write");
}

static void cat(Fs *f, const char *path)
{
    char buf[FS_MAX_FILE + 1];
    int n = fs_read(f, path, 0, buf, FS_MAX_FILE);
    if (n < 0) {
        printf("  %-20s (no such file)\n", path);
        return;
    }
    buf[n] = '\0';
    FsStat st;
    fs_stat(f, path, &st);
    printf("  %-20s inode %-3u \"%s\"\n", path, st.ino, buf);
}

static void footer(void)
{
    FsInfo in;
    fs_info(fs, &in);
    printf("disk blocks written: %llu   disk blocks in use: %u   free inodes: %u   "
           "fsck: %s   reference counts: %s\n",
           (unsigned long long)(disk_write_count(disk) - writes_before), vol_used_blocks(vol),
           in.inodes_free, fs_check(fs) ? "PROBLEMS" : "clean", vol_check(vol) ? "WRONG" : "OK");
}

int main(void)
{
    disk = mem_disk_create(2048);
    vol = disk ? vol_create(disk) : NULL;
    if (!vol || fs_format(vol) < 0 || !(fs = fs_open(vol)))
        return 1;

    step("1. Make some files (mkdir, create, write)");
    must(fs_mkdir(fs, "/docs"), "mkdir");
    put("/docs/report.txt", "v1: first draft");
    put("/docs/notes.txt", "meeting at 10");
    cat(fs, "/docs/report.txt");
    cat(fs, "/docs/notes.txt");
    footer();

    step("2. Take checkpoint \"monday\"");
    must(ckpt_create(vol, "monday"), "checkpoint");
    footer();
    printf("-> 0 blocks written: the checkpoint is a second pointer to the same tree.\n");

    step("3. Update report.txt, delete notes.txt, create todo.txt");
    put("/docs/report.txt", "v2: FINAL version");
    must(fs_unlink(fs, "/docs/notes.txt"), "unlink");
    put("/docs/todo.txt", "buy milk");
    cat(fs, "/docs/report.txt");
    cat(fs, "/docs/notes.txt");
    cat(fs, "/docs/todo.txt");
    footer();
    printf("-> ifree put notes.txt's inode back in the free list, and ialloc handed\n"
           "   the same inode to todo.txt. Every changed block went to a new disk\n"
           "   block, so monday's blocks are untouched.\n");

    step("4. Look inside \"monday\" (read-only)");
    Fs *old = ckpt_open_fs(vol, "monday");
    cat(old, "/docs/report.txt");
    cat(old, "/docs/notes.txt");
    cat(old, "/docs/todo.txt");
    printf("  fsck of monday's filesystem: %s\n", fs_check(old) ? "PROBLEMS" : "clean");
    fs_close(old);

    step("5. Roll back to \"monday\"");
    must(ckpt_rollback(vol, "monday"), "rollback");
    cat(fs, "/docs/report.txt");
    cat(fs, "/docs/notes.txt");
    cat(fs, "/docs/todo.txt");
    footer();
    printf("-> files, directories, inodes and both free lists are back as they were.\n");

    step("6. Delete \"monday\"");
    must(ckpt_delete(vol, "monday"), "delete");
    footer();
    printf("-> nothing freed: the live filesystem uses every block monday had.\n");

    fs_close(fs);
    vol_destroy(vol);
    disk_close(disk);
    return 0;
}
