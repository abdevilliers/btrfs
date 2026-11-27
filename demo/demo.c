/* demo.c - a narrated walk through every feature.
 *   ./build/demo          run straight through
 *   ./build/demo --pause  wait for Enter between steps (for recording) */
#include <stdio.h>
#include <string.h>
#include "fs.h"
#include "checkpoint.h"
#include "duplication.h"
#include "recovery.h"

static int pause_mode;
static Volume *v;

static void step(const char *title)
{
    if (pause_mode) { printf("\n[press Enter]"); fflush(stdout); getchar(); }
    printf("\n==== %s ====\n", title);
}

static void show(const CowTree *t, const char *label)
{
    Inode l[MAX_FILES];
    int n = fs_list(t, l);
    printf("  %s:\n", label);
    for (int i = 0; i < n; i++) {
        char buf[64] = {0};
        fs_read(t, l[i].name, buf, sizeof buf - 1);
        printf("    ino %u  %-10s %4u B  \"%s\"\n", l[i].ino, l[i].name, l[i].size, buf);
    }
    if (!n) puts("    (empty)");
}

static void write_file(const char *name, const char *text)
{
    unsigned long c0 = cowtree_copies;
    int n = fs_write(v, name, text, (uint32_t)strlen(text));
    printf("  write %-10s \"%s\"  -> %d data block(s), %lu tree node(s) copied\n",
           name, text, n, cowtree_copies - c0);
}

int main(int argc, char **argv)
{
    pause_mode = argc > 1 && !strcmp(argv[1], "--pause");
    Disk *d = ramdisk_create();

    step("1. Format: superblock + empty tree");
    v = rec_format(d);
    printf("  superblock at block %u, mirror at block %u, generation %llu\n",
           SB_PRIMARY, SB_MIRROR, (unsigned long long)v->generation);

    step("2. Files and inodes (write / read)");
    write_file("notes.txt", "version 1");
    write_file("todo.txt", "buy milk");
    show(&v->live, "live filesystem");

    step("3. Checkpoints: one per version");
    char name[8], text[16];
    for (int i = 1; i <= 4; i++) {
        snprintf(name, sizeof name, "c%d", i);
        unsigned long w0 = d->writes;
        ckpt_create(v, name);
        printf("  checkpoint %s taken (%lu blocks written)\n", name, d->writes - w0);
        snprintf(text, sizeof text, "version %d", i + 1);
        write_file("notes.txt", text);
    }
    RootEntry l[MAX_CKPT];
    int n = ckpt_list(v, l);
    for (int i = 0; i < n; i++) {
        CowTree t = vol_tree(v, l[i].root);
        char buf[32] = {0};
        fs_read(&t, "notes.txt", buf, sizeof buf - 1);
        printf("  %d back = %s : notes.txt = \"%s\"\n", i + 1, l[i].name, buf);
    }
    show(&v->live, "live filesystem");

    step("4. Roll back 3 checkpoints (not just the last one)");
    ckpt_rollback_back(v, 3, 1);
    show(&v->live, "live after rollback 3 back (= c2)");
    puts("  newer checkpoints are kept, so we can roll forward to c4 too:");
    ckpt_rollback(v, "c4", 1);
    show(&v->live, "live after rollback to c4");
    printf("  vol_check() = %d\n", vol_check(v));

    step("5. Duplication: reflink copy shares data blocks");
    static char big[3 * BLOCK_SIZE];
    memset(big, 'A', sizeof big);
    fs_write(v, "big.bin", big, sizeof big);
    uint32_t b0 = cowtree_get(&v->live, fs_data_page(fs_lookup(&v->live, "big.bin", NULL), 0));
    unsigned long w0 = d->writes;
    dup_copy_file(v, "big.bin", "copy.bin");
    printf("  cp --reflink big.bin copy.bin: %lu block writes (metadata only), page 0 block %u refs = %u\n",
           d->writes - w0, b0, alloc_refs(v->alloc, b0));
    big[0] = 'B';
    fs_write(v, "copy.bin", big, sizeof big);
    int cs = fs_lookup(&v->live, "copy.bin", NULL);
    printf("  write page 0 of copy.bin -> gets its own block %u; block %u refs now %u\n",
           cowtree_get(&v->live, fs_data_page(cs, 0)), b0, alloc_refs(v->alloc, b0));
    printf("  pages 1-2 still shared: refs = %u, %u\n",
           alloc_refs(v->alloc, cowtree_get(&v->live, fs_data_page(cs, 1))),
           alloc_refs(v->alloc, cowtree_get(&v->live, fs_data_page(cs, 2))));
    printf("  vol_check() = %d\n", vol_check(v));

    step("6. Recovery: commit, change things, crash, mount");
    rec_commit(v);
    printf("  committed generation %llu\n", (unsigned long long)v->generation);
    write_file("notes.txt", "UNSAVED change");
    fs_delete(v, "todo.txt");
    ckpt_delete(v, "c1");
    show(&v->live, "live before crash");
    puts("  *** crash: the in-memory volume is lost ***");
    vol_free(v);
    int which = rec_mount(d, &v);
    printf("  mounted generation %llu from the %s superblock\n",
           (unsigned long long)v->generation, which == 0 ? "primary" : "mirror");
    show(&v->live, "live after mount");
    printf("  checkpoints back: %d, vol_check() = %d\n", ckpt_list(v, l), vol_check(v));

    step("7. Recovery: corrupted superblock -> mirror");
    uint8_t junk[BLOCK_SIZE];
    memset(junk, 0xAB, sizeof junk);
    disk_write(d, SB_PRIMARY, junk);
    vol_free(v);
    which = rec_mount(d, &v);
    printf("  primary superblock corrupted; mounted from the %s, generation %llu\n",
           which == 0 ? "primary" : "mirror", (unsigned long long)v->generation);
    show(&v->live, "live after mount");

    vol_free(v);
    disk_destroy(d);
    puts("\ndemo finished");
    return 0;
}
