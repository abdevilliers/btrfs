/*
 * fsh.c - a small shell for the filesystem, kept in a disk-image file.
 *
 *     build/bin/fsh [image]          default image: fsh.img
 *
 * The image survives between runs: the shell mounts the last commit
 * (recovery), or formats a new filesystem if there is none. "exit"
 * commits first, like unmounting; "crash" quits without committing.
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
#include "volume.h"

#define BLOCKS 4096 /* 4096 x 128 bytes = a 512 KiB disk */

static Disk *disk;
static Volume *vol;
static Fs *fs;

static void help(void)
{
    puts("files and directories:\n"
         "  ls [dir]              list a directory\n"
         "  mkdir DIR             make a directory\n"
         "  touch FILE            make an empty file\n"
         "  write FILE TEXT...    replace a file's contents (creates it)\n"
         "  append FILE TEXT...   add to the end of a file\n"
         "  cat FILE              print a file\n"
         "  rm FILE               remove a file\n"
         "  rmdir DIR             remove an empty directory\n"
         "  ln FILE NEWNAME       hard link\n"
         "  cp FILE NEWNAME       copy without copying data (duplication)\n"
         "  truncate FILE SIZE    set a file's size\n"
         "  stat PATH             inode, type, links, size, pages\n"
         "  df                    free inodes and blocks\n"
         "  sb                    the superblock's free inode and free block lists\n"
         "  fsck                  check the filesystem\n"
         "checkpoints:\n"
         "  snap NAME             take a checkpoint\n"
         "  snaps                 list checkpoints, newest first (x = 1, 2, ...)\n"
         "  back X                go back X checkpoints\n"
         "  rollback NAME         go back to a checkpoint by name\n"
         "  delsnap NAME          delete a checkpoint\n"
         "  catsnap NAME FILE     print a file as it was in a checkpoint\n"
         "recovery:\n"
         "  commit                make everything so far survive a crash\n"
         "  exit                  commit and quit\n"
         "  crash                 quit WITHOUT committing (simulates a power cut)");
}

static void report(int r)
{
    if (r < 0)
        printf("error: %s\n", strerror(-r));
}

static void list_entry(void *arg, const char *name, const FsStat *st)
{
    (void)arg;
    printf("  %-4s %3u  %5u  %s%s\n", st->type == FS_DIR ? "dir" : "file", st->ino, st->size, name,
           st->type == FS_DIR ? "/" : "");
}

static void cat(Fs *f, const char *path)
{
    char buf[FS_MAX_FILE + 1];
    int n = f ? fs_read(f, path, 0, buf, FS_MAX_FILE) : -ENOENT;
    if (n < 0) {
        report(n);
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
    uint32_t off = append ? st.size : 0;
    r = fs_write(fs, path, off, text, (uint32_t)strlen(text));
    return r < 0 ? r : 0;
}

static void mount_image(const char *path)
{
    disk = file_disk_open(path, BLOCKS);
    if (!disk) {
        perror(path);
        exit(1);
    }
    if (rec_mount(disk, &vol) == 0) {
        printf("mounted %s (commit %u)\n", path, rec_generation(disk));
    } else {
        vol = vol_create(disk);
        if (!vol || fs_format(vol) < 0 || rec_commit(vol) < 0) {
            fprintf(stderr, "cannot format %s\n", path);
            exit(1);
        }
        printf("formatted a new filesystem in %s\n", path);
    }
    fs = fs_open(vol);
    if (!fs) {
        fprintf(stderr, "%s holds no filesystem\n", path);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    const char *image = argc > 1 ? argv[1] : "fsh.img";
    char line[512];

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
        char *rest = strtok(NULL, ""); /* everything after the second word */
        char *b = rest;
        if (!cmd)
            continue;
        if (strcmp(cmd, "#") == 0 || cmd[0] == '#')
            continue; /* a comment, for scripts */
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
            report(put(a, b ? b : "", cmd[0] == 'a'));
        } else if (strcmp(cmd, "cat") == 0 && a) {
            cat(fs, a);
        } else if (strcmp(cmd, "rm") == 0 && a) {
            report(fs_unlink(fs, a));
        } else if (strcmp(cmd, "rmdir") == 0 && a) {
            report(fs_rmdir(fs, a));
        } else if (strcmp(cmd, "ln") == 0 && a && b) {
            report(fs_link(fs, a, b));
        } else if (strcmp(cmd, "cp") == 0 && a && b) {
            report(dup_copy_file(fs, a, b));
        } else if (strcmp(cmd, "truncate") == 0 && a && b) {
            report(fs_truncate(fs, a, (uint32_t)strtoul(b, NULL, 10)));
        } else if (strcmp(cmd, "stat") == 0 && a) {
            FsStat st;
            int r = fs_stat(fs, a, &st);
            if (r == 0)
                printf("  inode %u  %s  links %u  size %u bytes  blocks %u\n", st.ino,
                       st.type == FS_DIR ? "directory" : "file", st.nlink, st.size, st.blocks);
            report(r);
        } else if (strcmp(cmd, "df") == 0) {
            FsInfo in;
            fs_info(fs, &in);
            printf("  inodes: %u free of %u   data blocks: %u free of %u   disk blocks in use: %u\n",
                   in.inodes_free, in.inodes_total, in.blocks_free, in.blocks_total, vol_used_blocks(vol));
        } else if (strcmp(cmd, "sb") == 0) {
            FsInfo in;
            fs_info(fs, &in);
            printf("  free inode list (ialloc takes from the right):");
            for (uint32_t k = 0; k < in.ninode; k++)
                printf(" %u", in.inode_list[k]);
            printf("\n  remembered inode: %u   free inodes in total: %u\n", in.remembered_inode,
                   in.inodes_free);
            printf("  free block list (alloc takes from the right): [link %u]", in.free_list[0]);
            for (uint32_t k = 1; k < in.nfree; k++)
                printf(" %u", in.free_list[k]);
            printf("\n  free blocks in total: %u\n", in.blocks_free);
        } else if (strcmp(cmd, "fsck") == 0) {
            int e = fs_check(fs), r = vol_check(vol);
            printf("  filesystem: %s   reference counts: %s\n", e ? "PROBLEMS" : "clean", r ? "WRONG" : "OK");
        } else if (strcmp(cmd, "snap") == 0 && a) {
            report(ckpt_create(vol, a));
        } else if (strcmp(cmd, "snaps") == 0) {
            char name[CKPT_NAME_MAX + 1];
            for (uint32_t x = 1; ckpt_name_back(vol, x, name) == 0; x++)
                printf("  x = %u  %s\n", x, name);
        } else if (strcmp(cmd, "back") == 0 && a) {
            report(ckpt_rollback_back(vol, (uint32_t)strtoul(a, NULL, 10)));
        } else if (strcmp(cmd, "rollback") == 0 && a) {
            report(ckpt_rollback(vol, a));
        } else if (strcmp(cmd, "delsnap") == 0 && a) {
            report(ckpt_delete(vol, a));
        } else if (strcmp(cmd, "catsnap") == 0 && a && b) {
            Fs *old = ckpt_open_fs(vol, a);
            cat(old, b);
            fs_close(old);
        } else if (strcmp(cmd, "commit") == 0) {
            report(rec_commit(vol));
            printf("  commit %u\n", rec_generation(disk));
        } else if (strcmp(cmd, "crash") == 0) {
            puts("  power cut: nothing since the last commit is kept");
            fs_close(fs);
            vol_destroy(vol);
            disk_close(disk);
            return 0;
        } else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            break;
        } else {
            puts("unknown command or missing argument; type \"help\"");
        }
    }
    report(rec_commit(vol)); /* unmount: make it all durable */
    fs_close(fs);
    vol_destroy(vol);
    disk_close(disk);
    return 0;
}
