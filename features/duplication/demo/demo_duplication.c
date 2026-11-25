/*
 * demo_duplication.c - copying a file without copying its data
 * (cp --reflink). For each file it prints its extents (FIEMAP) and, for
 * each data extent, the extent item's reference count and its
 * back-references from the extent tree.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "duplication.h"
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
    btrfs_commit(b); /* run the delayed refs, so the extent tree is current */
    btrfs_stats(b, &before);
}

/* Print the extent item and back-references of one data extent. */
static int show_ref(void *arg, const BtrfsKey *k, const void *data, uint32_t len)
{
    (void)arg;
    (void)len;
    if (k->type == BTRFS_EXTENT_ITEM_KEY) {
        BtrfsExtentItem ei;
        memcpy(&ei, data, sizeof ei);
        printf("refs %llu:", (unsigned long long)ei.refs);
    } else if (k->type == BTRFS_EXTENT_DATA_REF_KEY) {
        BtrfsExtentDataRef dr;
        memcpy(&dr, data, sizeof dr);
        printf(" [subvol %llu inode %llu x%u]", (unsigned long long)dr.root, (unsigned long long)dr.objectid,
               dr.count);
    }
    return 0;
}

static void show_extent(void *arg, const FsExtent *e)
{
    (void)arg;
    printf("    file bytes %6llu..%-6llu -> disk byte %-9llu ", (unsigned long long)e->logical,
           (unsigned long long)(e->logical + e->length), (unsigned long long)e->physical);
    if (e->flags & FIEMAP_EXTENT_INLINE) {
        printf("(inline)\n");
        return;
    }
    /* The extent item and back-references of the extent it points into. */
    BtrfsKey lo = { e->extent_bytenr, 0, 0 }, hi = { e->extent_bytenr, 0xff, (uint64_t)-1 };
    btrfs_tree_search(b, BTRFS_EXTENT_TREE_OBJECTID, &lo, &hi, show_ref, NULL);
    printf("%s\n", e->flags & FIEMAP_EXTENT_SHARED ? "  SHARED" : "");
}

static void show(const char *path)
{
    FsStat st;
    must(fs_stat(fs, path, &st), "stat");
    printf("  %s: inode %llu, %llu bytes\n", path, (unsigned long long)st.ino, (unsigned long long)st.size);
    fs_fiemap(fs, path, show_extent, NULL);
}

static void footer(void)
{
    BtrfsStats now;
    FsSpace sp;
    btrfs_commit(b);
    btrfs_stats(b, &now);
    fs_space(b, &sp);
    printf("data sectors written: %llu   data in extents: %llu bytes   btrfs check: %s\n",
           (unsigned long long)(now.data_sectors_written - before.data_sectors_written),
           (unsigned long long)sp.data_used, btrfs_check(disk, 0) ? "PROBLEMS" : "clean");
}

int main(void)
{
    static char text[20000];
    BtrfsMkfsOpts o = { 4096, NULL };
    disk = mem_disk_create(16384);
    if (!disk || btrfs_mkfs(disk, &o) < 0 || btrfs_mount(disk, NULL, &b) < 0 || !(fs = fs_open(b)))
        return 1;
    for (int i = 0; i < (int)sizeof text; i++)
        text[i] = (char)('A' + i / (int)BLOCK_SIZE);

    step("1. Write a 20000-byte file");
    must(fs_create(fs, "/report.txt"), "create");
    must((int)fs_write(fs, "/report.txt", 0, text, sizeof text), "write");
    footer();
    show("/report.txt");

    step("2. cp --reflink /report.txt /copy.txt  (dup_copy_file)");
    must(dup_copy_file(fs, "/report.txt", "/copy.txt"), "copy");
    footer();
    show("/report.txt");
    show("/copy.txt");
    printf("-> the copy has its own inode and EXTENT_DATA item pointing at the SAME extent:\n"
           "   refs 2, one EXTENT_DATA_REF per file. No data was written.\n");

    step("3. Update the copy: write \"xyz\" at byte 5000");
    must((int)fs_write(fs, "/copy.txt", 5000, "xyz", 3), "write");
    footer();
    show("/report.txt");
    show("/copy.txt");
    printf("-> the written sector went to a new extent; the copy's item was split around it\n"
           "   (two items into the old extent: that is why its ref count is 2 there).\n");

    step("4. Delete the original");
    must(fs_unlink(fs, "/report.txt"), "unlink");
    footer();
    show("/copy.txt");
    printf("-> the old extent stays: the copy still references it.\n");

    fs_close(fs);
    btrfs_unmount(b);
    disk_close(disk);
    return 0;
}
