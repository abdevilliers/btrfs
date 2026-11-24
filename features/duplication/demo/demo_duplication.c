/*
 * demo_duplication.c - copying a file without copying its data.
 *
 * A file is written with the normal file operations, then copied with
 * dup_copy_file (cp --reflink). The copy gets its own inode (ialloc) and
 * its own filesystem blocks (alloc), but each of them points at the same
 * disk block as the original. Updating the copy splits only the block
 * that changes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "duplication.h"
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

/* Each block of a file: filesystem block -> disk block (reference count). */
static void show(const char *path)
{
    FsStat st;
    must(fs_stat(fs, path, &st), "stat");
    printf("  %s: inode %u, %u bytes\n", path, st.ino, st.size);
    for (uint32_t i = 0; i * BLOCK_SIZE < st.size; i++) {
        uint32_t b;
        must(fs_bmap(fs, path, i, 0, &b), "bmap");
        uint32_t d = cowtree_get(vol_tree(vol), vol_live_root(vol), b);
        printf("    file block %u -> fs block %-4u -> disk block #%-4u refs %u%s\n", i, b, d,
               alloc_refcount(vol_allocator(vol), d),
               alloc_refcount(vol_allocator(vol), d) > 1 ? "  shared" : "");
    }
}

static void footer(void)
{
    FsInfo in;
    fs_info(fs, &in);
    printf("disk blocks written: %llu   disk blocks in use: %u   free fs blocks: %u   "
           "free inodes: %u   fsck: %s\n",
           (unsigned long long)(disk_write_count(disk) - writes_before), vol_used_blocks(vol),
           in.blocks_free, in.inodes_free, fs_check(fs) ? "PROBLEMS" : "clean");
}

int main(void)
{
    char text[500];

    disk = mem_disk_create(2048);
    vol = disk ? vol_create(disk) : NULL;
    if (!vol || fs_format(vol) < 0 || !(fs = fs_open(vol)))
        return 1;
    for (int i = 0; i < (int)sizeof text; i++)
        text[i] = (char)('A' + i / (int)BLOCK_SIZE); /* block 0 = AAAA..., block 1 = BBBB..., ... */

    step("1. Write a 500-byte file (4 blocks)");
    must(fs_create(fs, "/report.txt"), "create");
    must(fs_write(fs, "/report.txt", 0, text, sizeof text), "write");
    show("/report.txt");
    footer();

    step("2. cp /report.txt /copy.txt  (dup_copy_file)");
    must(dup_copy_file(fs, "/report.txt", "/copy.txt"), "copy");
    show("/report.txt");
    show("/copy.txt");
    footer();
    printf("-> the copy has its own inode and its own fs blocks, but every one points at\n"
           "   the original's disk block (refs 2). No data was copied.\n");

    step("3. Update the copy: write \"xyz\" at byte 130 (file block 1)");
    must(fs_write(fs, "/copy.txt", 130, "xyz", 3), "write");
    show("/report.txt");
    show("/copy.txt");
    footer();
    printf("-> only file block 1 of the copy got a new disk block; the rest stay shared.\n");

    step("4. Delete the original");
    must(fs_unlink(fs, "/report.txt"), "unlink");
    show("/copy.txt");
    footer();
    printf("-> the copy keeps its data: a disk block is freed only at refs 0.\n");

    fs_close(fs);
    vol_destroy(vol);
    disk_close(disk);
    return 0;
}
