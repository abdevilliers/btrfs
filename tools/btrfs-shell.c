/* btrfs-shell - an interactive shell over a file-backed toy btrfs.
 *   ./build/btrfs-shell [disk.img]
 * Type `help` for commands. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fs.h"
#include "checkpoint.h"
#include "duplication.h"
#include "recovery.h"

static Disk *disk;
static Volume *v;
static const char *path;

static void help(void)
{
    puts(
    "files\n"
    "  ls                          list files (inode, size, name)\n"
    "  write <file> <text...>      create/overwrite a file with text\n"
    "  fill <file> <kb> <char>     write a <kb> KiB file of one character\n"
    "  cat <file>                  print a file\n"
    "  rm <file>                   delete a file\n"
    "  blocks <file>               show each page's block and refcount\n"
    "checkpointing\n"
    "  checkpoint <name>           take a checkpoint (0 blocks copied)\n"
    "  checkpoints                 list checkpoints, newest = 1 back\n"
    "  rollback <N>                roll back to the checkpoint N back\n"
    "  rollback-to <name>          roll back to a named checkpoint\n"
    "  rollback-drop <N>           roll back N and delete newer checkpoints\n"
    "  ckpt-ls <name>              list files inside a checkpoint\n"
    "  ckpt-cat <name> <file>      print a file as it was in a checkpoint\n"
    "  ckpt-rm <name>              delete a checkpoint\n"
    "duplication\n"
    "  cp <src> <dst>              reflink copy: shares data blocks\n"
    "recovery\n"
    "  commit                      make the current state durable\n"
    "  crash                       quit WITHOUT committing (simulated power loss)\n"
    "  corrupt-sb                  scribble over the primary superblock\n"
    "  exit                        commit and quit\n"
    "other\n"
    "  info                        superblock, space and copy statistics\n"
    "  check                       verify every refcount (fsck)\n"
    "  format                      wipe and create an empty filesystem");
}

static void info(void)
{
    printf("generation (last commit) : %llu\n", (unsigned long long)v->generation);
    printf("superblock slot          : %u\n", v->sb_slot);
    printf("live root block          : %u (refs %u)\n", v->live.root,
           v->live.root ? alloc_refs(v->alloc, v->live.root) : 0);
    printf("blocks in use            : %u of %u\n", alloc_used(v->alloc), NBLOCKS - FIRST_FREE);
    printf("nodes copied / in place  : %lu / %lu\n", cowtree_copies, cowtree_inplace);
    printf("disk reads / writes      : %lu / %lu\n", disk->reads, disk->writes);
}

static void ls_tree(const CowTree *t)
{
    Inode list[MAX_FILES];
    int n = fs_list(t, list);
    if (!n) puts("(empty)");
    for (int i = 0; i < n; i++)
        printf("  ino %u  %6u B  %s\n", list[i].ino, list[i].size, list[i].name);
}

static void cat_tree(const CowTree *t, const char *name)
{
    static char buf[MAX_FILE_SIZE + 1];
    long n = fs_read(t, name, buf, MAX_FILE_SIZE);
    if (n < 0) { printf("no such file: %s\n", name); return; }
    buf[n] = 0;
    if (n > 300) printf("%.300s... (%ld bytes)\n", buf, n);
    else         printf("%s\n", buf);
}

static void show_blocks(const char *name)
{
    Inode ino;
    int slot = fs_lookup(&v->live, name, &ino);
    if (slot < 0) { printf("no such file: %s\n", name); return; }
    printf("%s (ino %u, %u bytes)\n", ino.name, ino.ino, ino.size);
    for (int k = 0; k < PAGES_PER_FILE; k++) {
        uint32_t b = cowtree_get(&v->live, fs_data_page(slot, k));
        if (b) printf("  page %2d -> block %4u   refs %u%s\n", k, b,
                      alloc_refs(v->alloc, b), alloc_refs(v->alloc, b) > 1 ? "  (shared)" : "");
    }
}

static void list_ckpts(void)
{
    RootEntry l[MAX_CKPT];
    int n = ckpt_list(v, l);
    if (!n) { puts("(no checkpoints)"); return; }
    puts("  back  id  gen  root  name");
    for (int i = 0; i < n; i++)
        printf("  %4d  %2u  %3llu  %4u  %s\n", i + 1, l[i].id,
               (unsigned long long)l[i].generation, l[i].root, l[i].name);
}

static void mount_or_format(void)
{
    int existed;
    disk = filedisk_open(path, &existed);
    if (!disk) { perror(path); exit(1); }
    int r = existed ? rec_mount(disk, &v) : -1;
    if (r < 0) {
        v = rec_format(disk);
        printf("formatted new filesystem on %s\n", path);
    } else {
        printf("mounted %s: generation %llu from the %s superblock\n", path,
               (unsigned long long)v->generation, r == 0 ? "primary" : "MIRROR");
    }
}

int main(int argc, char **argv)
{
    path = argc > 1 ? argv[1] : "disk.img";
    mount_or_format();
    puts("toy btrfs shell - type `help`");

    char line[4096];
    for (;;) {
        printf("btrfs> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        line[strcspn(line, "\n")] = 0;
        char *cmd = strtok(line, " ");
        if (!cmd) continue;
        char *a1 = strtok(NULL, " ");
        char *rest = strtok(NULL, "");           /* rest of the line */
        char *a2 = rest;

        if (!strcmp(cmd, "help")) help();
        else if (!strcmp(cmd, "ls")) ls_tree(&v->live);
        else if (!strcmp(cmd, "write") && a1) {
            const char *text = rest ? rest : "";
            int n = fs_write(v, a1, text, (uint32_t)strlen(text));
            if (n < 0) puts("write failed (name too long, too big, or 15 files)");
            else printf("wrote %s: %d new data block(s)\n", a1, n);
        }
        else if (!strcmp(cmd, "fill") && a1 && a2) {
            int kb = atoi(strtok(a2, " "));
            char *c = strtok(NULL, " ");
            if (kb <= 0 || (uint32_t)kb * 1024 > MAX_FILE_SIZE) { puts("size must be 1..64 KiB"); continue; }
            char *buf = malloc((size_t)kb * 1024);
            memset(buf, c ? c[0] : 'x', (size_t)kb * 1024);
            int n = fs_write(v, a1, buf, (uint32_t)kb * 1024);
            free(buf);
            printf("wrote %s: %d new data block(s)\n", a1, n);
        }
        else if (!strcmp(cmd, "cat") && a1) cat_tree(&v->live, a1);
        else if (!strcmp(cmd, "rm") && a1) puts(fs_delete(v, a1) ? "no such file" : "deleted");
        else if (!strcmp(cmd, "blocks") && a1) show_blocks(a1);
        else if (!strcmp(cmd, "checkpoint") && a1) {
            unsigned long before = disk->writes;
            if (ckpt_create(v, a1)) puts("failed (exists, bad name, or 16 already)");
            else printf("checkpoint '%s' created (%lu blocks copied)\n", a1, disk->writes - before);
        }
        else if (!strcmp(cmd, "checkpoints")) list_ckpts();
        else if (!strcmp(cmd, "rollback") && a1) {
            if (ckpt_rollback_back(v, atoi(a1), 1)) puts("no such checkpoint");
            else printf("live filesystem is now checkpoint %d back\n", atoi(a1));
        }
        else if (!strcmp(cmd, "rollback-drop") && a1) {
            if (ckpt_rollback_back(v, atoi(a1), 0)) puts("no such checkpoint");
            else printf("rolled back %d checkpoint(s), newer ones deleted\n", atoi(a1));
        }
        else if (!strcmp(cmd, "rollback-to") && a1)
            puts(ckpt_rollback(v, a1, 1) ? "no such checkpoint" : "rolled back");
        else if (!strcmp(cmd, "ckpt-ls") && a1) {
            CowTree t;
            if (ckpt_read(v, a1, &t)) puts("no such checkpoint"); else ls_tree(&t);
        }
        else if (!strcmp(cmd, "ckpt-cat") && a1 && a2) {
            CowTree t;
            if (ckpt_read(v, a1, &t)) puts("no such checkpoint"); else cat_tree(&t, a2);
        }
        else if (!strcmp(cmd, "ckpt-rm") && a1) puts(ckpt_delete(v, a1) ? "no such checkpoint" : "deleted");
        else if (!strcmp(cmd, "cp") && a1 && a2) {
            unsigned long before = alloc_used(v->alloc);
            if (dup_copy_file(v, a1, a2)) puts("copy failed");
            else printf("reflinked %s -> %s (0 data blocks copied, %ld new metadata blocks)\n", a1, a2,
                        (long)alloc_used(v->alloc) - (long)before);
        }
        else if (!strcmp(cmd, "commit")) {
            rec_commit(v);
            printf("committed generation %llu\n", (unsigned long long)v->generation);
        }
        else if (!strcmp(cmd, "crash")) { puts("*** simulated crash: nothing since the last commit is saved ***"); exit(0); }
        else if (!strcmp(cmd, "corrupt-sb")) {
            uint8_t junk[BLOCK_SIZE];
            memset(junk, 0xAB, sizeof junk);
            disk_write(disk, SB_PRIMARY, junk);
            disk_flush(disk);
            puts("primary superblock overwritten with garbage");
        }
        else if (!strcmp(cmd, "info")) info();
        else if (!strcmp(cmd, "check")) {
            int bad = vol_check(v);
            printf(bad ? "check: %d block(s) with wrong refcount\n" : "check: OK, every refcount matches\n", bad);
        }
        else if (!strcmp(cmd, "format")) {
            vol_free(v);
            v = rec_format(disk);
            puts("formatted");
        }
        else if (!strcmp(cmd, "exit") || !strcmp(cmd, "quit")) break;
        else puts("unknown command or missing argument (try `help`)");
    }
    rec_commit(v);
    printf("committed generation %llu, bye\n", (unsigned long long)v->generation);
    vol_free(v);
    disk_destroy(disk);
    return 0;
}
