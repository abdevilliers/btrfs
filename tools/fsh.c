/*
 * fsh.c - a small shell for the filesystem, kept in a disk-image file.
 *
 *     build/bin/fsh [image]          default image: fsh.img
 *
 * The image survives between runs: the shell mounts its last commit, or
 * makes a new filesystem (mkfs + the checkpoint layout) if there is none.
 * "exit" commits first, like unmounting; "crash" quits without committing.
 * Type "help" for the commands.
 */
#define _POSIX_C_SOURCE 200809L /* isatty */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "checkpoint.h"
#include "duplication.h"
#include "file_disk.h"
#include "fs.h"
#include "recovery.h"

#define BLOCKS 32768 /* 32768 x 4 KiB = a 128 MiB disk (with the 64 MiB superblock copy) */

static Disk *disk;
static Btrfs *b;
static Fs *fs;

static void help(void)
{
    puts("files and directories (in the live subvolume):\n"
         "  ls [dir]              list a directory\n"
         "  mkdir DIR             make a directory\n"
         "  touch FILE            make an empty file\n"
         "  write FILE TEXT...    replace a file's contents (creates it)\n"
         "  append FILE TEXT...   add to the end of a file\n"
         "  cat FILE              print a file\n"
         "  rm FILE / rmdir DIR   remove\n"
         "  ln FILE NEWNAME       hard link\n"
         "  cp FILE NEWNAME       reflink copy: shares the data (duplication)\n"
         "  truncate FILE SIZE    set a file's size\n"
         "  stat PATH             inode, subvolume, links, size, nbytes\n"
         "  fiemap FILE           a file's extents, and which are shared\n"
         "checkpoints (read-only snapshots of the live subvolume):\n"
         "  snap NAME             take a checkpoint (commits)\n"
         "  snaps                 list checkpoints, newest first (x = 1, 2, ...)\n"
         "  back X                go back X checkpoints\n"
         "  rollback NAME         go back to a checkpoint by name\n"
         "  delsnap NAME          delete a checkpoint\n"
         "  catsnap NAME FILE     print a file as it was in a checkpoint\n"
         "btrfs:\n"
         "  subvol list           btrfs subvolume list\n"
         "  df                    btrfs filesystem df\n"
         "  dump-super            the superblock and its backup roots\n"
         "  dump-tree ID          every block and item of a tree: 1 root, 2 extent,\n"
         "                        5 top level, 7 csum, 9 uuid, 10 free space, 256+ subvolumes\n"
         "  check                 btrfs check of the committed state\n"
         "  commit                commit the running transaction\n"
         "  exit                  commit and quit\n"
         "  crash                 quit WITHOUT committing (a power cut)");
}

static void report(int r)
{
    if (r < 0)
        printf("error: %s\n", strerror(-r));
}

static void list_entry(void *arg, const char *name, const FsStat *st)
{
    (void)arg;
    printf("  %-4s %5llu  %8llu  %s%s\n", st->type == FS_DIR ? "dir" : "file", (unsigned long long)st->ino,
           (unsigned long long)st->size, name, st->type == FS_DIR ? "/" : "");
}

static void cat(Fs *f, const char *path)
{
    char buf[65536];
    int64_t n = f ? fs_read(f, path, 0, buf, sizeof buf) : -ENOENT;
    if (n < 0) {
        report((int)n);
        return;
    }
    fwrite(buf, 1, (size_t)n, stdout);
    if (n == 0 || buf[n - 1] != '\n')
        putchar('\n');
}

static int put(const char *path, const char *text, int append)
{
    FsStat st;
    int r = fs_create(fs, path);
    if (r < 0 && r != -EEXIST)
        return r;
    if ((r = fs_stat(fs, path, &st)) < 0)
        return r;
    if (!append && (r = fs_truncate(fs, path, 0)) < 0)
        return r;
    int64_t w = fs_write(fs, path, append ? st.size : 0, text, strlen(text));
    return w < 0 ? (int)w : 0;
}

static void show_extent(void *arg, const FsExtent *e)
{
    (void)arg;
    printf("  %8llu..%-8llu -> %s %-10llu %s\n", (unsigned long long)e->logical,
           (unsigned long long)(e->logical + e->length), e->flags & FIEMAP_EXTENT_INLINE ? "inline" : "disk",
           (unsigned long long)e->physical, e->flags & FIEMAP_EXTENT_SHARED ? "shared" : "");
}

static void show_subvol(void *arg, const FsSubvolInfo *i)
{
    (void)arg;
    printf("  ID %-4llu gen %-5llu parent %-4llu top level %llu path %s%s%s\n", (unsigned long long)i->id,
           (unsigned long long)i->generation, (unsigned long long)i->parent_id, (unsigned long long)i->parent_id,
           i->name, i->read_only ? "  (read-only)" : "", i->id == fs_get_default_subvol(b) ? "  (default)" : "");
}

static void reopen(void)
{
    fs_close(fs);
    fs = fs_open(b);
}

static void mount_image(const char *path)
{
    disk = file_disk_open(path, BLOCKS);
    if (!disk) {
        perror(path);
        exit(1);
    }
    int how;
    if (rec_mount(disk, &b, &how) == 0) {
        printf("mounted %s (generation %llu)\n", path, (unsigned long long)btrfs_generation(b));
    } else {
        BtrfsMkfsOpts o = { 4096, "fsh" };
        if (btrfs_mkfs(disk, &o) < 0 || btrfs_mount(disk, NULL, &b) < 0 || ckpt_setup(b) < 0) {
            fprintf(stderr, "cannot make a filesystem in %s\n", path);
            exit(1);
        }
        printf("made a new filesystem in %s\n", path);
    }
    fs = fs_open(b);
    if (!fs) {
        fprintf(stderr, "%s: no default subvolume\n", path);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    const char *image = argc > 1 ? argv[1] : "fsh.img";
    char line[1024];

    mount_image(image);
    puts("type \"help\" for the commands");
    for (;;) {
        printf("fsh> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            break;
        line[strcspn(line, "\n")] = '\0';
        if (!isatty(0))
            printf("%s\n", line); /* show the command when reading a script */

        char *cmd = strtok(line, " ");
        char *a = strtok(NULL, " ");
        char *rest = strtok(NULL, "");
        char *b2 = rest;
        if (!cmd || cmd[0] == '#')
            continue;
        if (strcmp(cmd, "help") == 0) {
            help();
        } else if (strcmp(cmd, "ls") == 0) {
            report(fs_readdir(fs, a ? a : "/", list_entry, NULL));
        } else if (strcmp(cmd, "mkdir") == 0 && a) {
            report(fs_mkdir(fs, a));
        } else if (strcmp(cmd, "touch") == 0 && a) {
            int r = fs_create(fs, a);
            report(r == -EEXIST ? 0 : r);
        } else if ((strcmp(cmd, "write") == 0 || strcmp(cmd, "append") == 0) && a) {
            report(put(a, b2 ? b2 : "", cmd[0] == 'a'));
        } else if (strcmp(cmd, "cat") == 0 && a) {
            cat(fs, a);
        } else if (strcmp(cmd, "rm") == 0 && a) {
            report(fs_unlink(fs, a));
        } else if (strcmp(cmd, "rmdir") == 0 && a) {
            report(fs_rmdir(fs, a));
        } else if (strcmp(cmd, "ln") == 0 && a && b2) {
            report(fs_link(fs, a, b2));
        } else if (strcmp(cmd, "cp") == 0 && a && b2) {
            report(dup_copy_file(fs, a, b2));
        } else if (strcmp(cmd, "truncate") == 0 && a && b2) {
            report(fs_truncate(fs, a, strtoull(b2, NULL, 10)));
        } else if (strcmp(cmd, "stat") == 0 && a) {
            FsStat st;
            int r = fs_stat(fs, a, &st);
            if (r == 0)
                printf("  subvolume %llu inode %llu  %s  links %u  size %llu  nbytes %llu  gen %llu\n",
                       (unsigned long long)st.subvol, (unsigned long long)st.ino,
                       st.type == FS_DIR ? "directory" : "file", st.nlink, (unsigned long long)st.size,
                       (unsigned long long)st.nbytes, (unsigned long long)st.generation);
            report(r);
        } else if (strcmp(cmd, "fiemap") == 0 && a) {
            report(fs_fiemap(fs, a, show_extent, NULL));
        } else if (strcmp(cmd, "df") == 0) {
            FsSpace s;
            fs_space(b, &s);
            printf("  Data:     total %llu, used %llu\n  Metadata: total %llu, used %llu\n"
                   "  device %llu bytes, unallocated %llu, %u block groups\n",
                   (unsigned long long)s.data_total, (unsigned long long)s.data_used,
                   (unsigned long long)s.metadata_total, (unsigned long long)s.metadata_used,
                   (unsigned long long)s.total_bytes, (unsigned long long)s.unallocated, s.block_groups);
        } else if (strcmp(cmd, "subvol") == 0 && a && strcmp(a, "list") == 0) {
            report(fs_subvol_list(b, show_subvol, NULL));
        } else if (strcmp(cmd, "dump-super") == 0) {
            btrfs_dump_super(b);
        } else if (strcmp(cmd, "dump-tree") == 0 && a) {
            if (btrfs_dump_tree(b, strtoull(a, NULL, 10)) < 0)
                puts("error: no such tree");
        } else if (strcmp(cmd, "check") == 0) {
            int e = btrfs_check(disk, 1);
            printf("  btrfs check (generation %llu): %s\n", (unsigned long long)btrfs_disk_generation(disk),
                   e ? "PROBLEMS" : "clean");
        } else if (strcmp(cmd, "snap") == 0 && a) {
            report(ckpt_create(b, a));
        } else if (strcmp(cmd, "snaps") == 0) {
            char name[CKPT_NAME_MAX + 1];
            for (uint32_t x = 1; ckpt_name_back(b, x, name) == 0; x++)
                printf("  x = %u  %s\n", x, name);
        } else if (strcmp(cmd, "back") == 0 && a) {
            report(ckpt_rollback_back(b, (uint32_t)strtoul(a, NULL, 10)));
            reopen();
        } else if (strcmp(cmd, "rollback") == 0 && a) {
            report(ckpt_rollback(b, a));
            reopen();
        } else if (strcmp(cmd, "delsnap") == 0 && a) {
            report(ckpt_delete(b, a));
        } else if (strcmp(cmd, "catsnap") == 0 && a && b2) {
            Fs *old = ckpt_open_fs(b, a);
            cat(old, b2);
            fs_close(old);
        } else if (strcmp(cmd, "commit") == 0) {
            report(rec_commit(b));
            printf("  generation %llu\n", (unsigned long long)btrfs_generation(b));
        } else if (strcmp(cmd, "crash") == 0) {
            puts("  power cut: nothing since the last commit is kept");
            fs_close(fs);
            btrfs_crash(b);
            disk_close(disk);
            return 0;
        } else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            break;
        } else {
            puts("unknown command or missing argument; type \"help\"");
        }
    }
    fs_close(fs);
    report(btrfs_unmount(b)); /* commits */
    disk_close(disk);
    return 0;
}
