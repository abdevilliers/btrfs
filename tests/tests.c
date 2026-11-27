/* tests.c - automated tests for core + all three features. */
#include <stdio.h>
#include <string.h>
#include "fs.h"
#include "checkpoint.h"
#include "duplication.h"
#include "recovery.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int read_eq(const CowTree *t, const char *name, const char *want)
{
    char buf[MAX_FILE_SIZE + 1];
    long n = fs_read(t, name, buf, MAX_FILE_SIZE);
    if (n < 0) return want == NULL;
    buf[n] = 0;
    return want && strcmp(buf, want) == 0;
}

static void test_files(void)
{
    puts("files and inodes");
    Disk *d = ramdisk_create();
    Volume *v = rec_format(d);
    CHECK(fs_write(v, "a.txt", "hello", 5) == 1);
    CHECK(fs_write(v, "b.txt", "world", 5) == 1);
    CHECK(read_eq(&v->live, "a.txt", "hello"));
    Inode ino;
    CHECK(fs_lookup(&v->live, "b.txt", &ino) == 1 && ino.ino == 258 && ino.size == 5);
    CHECK(fs_write(v, "a.txt", "hello", 5) == 0);          /* unchanged: no write */
    CHECK(fs_write(v, "a.txt", "HELLO again", 11) == 1);
    CHECK(read_eq(&v->live, "a.txt", "HELLO again"));

    static char big[3 * BLOCK_SIZE + 10];                   /* multi-page file */
    for (unsigned i = 0; i < sizeof big; i++) big[i] = (char)('a' + i % 26);
    CHECK(fs_write(v, "big", big, sizeof big) == 4);
    char out[sizeof big];
    CHECK(fs_read(&v->live, "big", out, sizeof out) == (long)sizeof big && memcmp(out, big, sizeof big) == 0);

    CHECK(fs_delete(v, "b.txt") == 0);
    CHECK(fs_lookup(&v->live, "b.txt", NULL) == -1);
    Inode list[MAX_FILES];
    CHECK(fs_list(&v->live, list) == 2);
    CHECK(vol_check(v) == 0);
    vol_free(v); disk_destroy(d);
}

static void test_checkpoints(void)
{
    puts("checkpoints: create, read, rollback N back, delete");
    Disk *d = ramdisk_create();
    Volume *v = rec_format(d);
    char name[16], text[32];
    for (int i = 1; i <= 5; i++) {                  /* v1..v5, checkpoint each */
        snprintf(text, sizeof text, "version %d", i);
        fs_write(v, "notes.txt", text, (uint32_t)strlen(text));
        snprintf(name, sizeof name, "c%d", i);
        CHECK(ckpt_create(v, name) == 0);
    }
    fs_write(v, "notes.txt", "version 6", 9);
    CHECK(vol_check(v) == 0);

    CowTree t;
    CHECK(ckpt_read(v, "c2", &t) == 0 && read_eq(&t, "notes.txt", "version 2"));

    RootEntry list[MAX_CKPT];
    CHECK(ckpt_list(v, list) == 5 && strcmp(list[0].name, "c5") == 0);

    CHECK(ckpt_rollback_back(v, 3, 1) == 0);        /* 3 back = c3 */
    CHECK(read_eq(&v->live, "notes.txt", "version 3"));
    CHECK(vol_check(v) == 0);
    CHECK(ckpt_rollback(v, "c5", 1) == 0);          /* roll forward again */
    CHECK(read_eq(&v->live, "notes.txt", "version 5"));
    CHECK(ckpt_rollback_back(v, 4, 0) == 0);        /* to c2, drop c3..c5 */
    CHECK(read_eq(&v->live, "notes.txt", "version 2"));
    CHECK(ckpt_list(v, list) == 2);

    fs_write(v, "notes.txt", "after rollback", 14);  /* checkpoint unaffected */
    CHECK(ckpt_read(v, "c2", &t) == 0 && read_eq(&t, "notes.txt", "version 2"));
    CHECK(ckpt_delete(v, "c1") == 0 && ckpt_delete(v, "c2") == 0);
    CHECK(vol_check(v) == 0);
    CHECK(ckpt_rollback_back(v, 1, 1) == -1);

    vol_free(v); disk_destroy(d);
}

static void test_duplication(void)
{
    puts("duplication: reflink copy shares blocks, splits on write");
    Disk *d = ramdisk_create();
    Volume *v = rec_format(d);
    fs_write(v, "a", "original data", 13);
    CHECK(dup_copy_file(v, "a", "b") == 0);
    uint32_t blk = cowtree_get(&v->live, fs_data_page(0, 0));
    CHECK(blk == cowtree_get(&v->live, fs_data_page(1, 0)));
    CHECK(alloc_refs(v->alloc, blk) == 2);
    CHECK(read_eq(&v->live, "b", "original data"));
    fs_write(v, "b", "changed", 7);
    CHECK(alloc_refs(v->alloc, blk) == 1);
    CHECK(read_eq(&v->live, "a", "original data"));
    CHECK(read_eq(&v->live, "b", "changed"));
    CHECK(vol_check(v) == 0);

    ckpt_create(v, "snap");                         /* works with checkpoints */
    dup_copy_file(v, "a", "c");
    fs_delete(v, "a");
    CHECK(read_eq(&v->live, "c", "original data"));
    CHECK(vol_check(v) == 0);
    vol_free(v); disk_destroy(d);
}

static void test_recovery(void)
{
    puts("recovery: crash test, torn superblock, file disk");
    Disk *d = ramdisk_create();
    Volume *v = rec_format(d);
    fs_write(v, "keep", "committed", 9);
    ckpt_create(v, "cp");
    rec_commit(v);
    uint64_t gen = v->generation;

    fs_write(v, "keep", "NOT committed", 13);       /* change things ... */
    fs_write(v, "lost", "gone", 4);
    ckpt_delete(v, "cp");
    for (int i = 0; i < 20; i++) fs_write(v, "churn", "x", 1 + (uint32_t)i);
    vol_free(v);                                    /* ... crash */

    CHECK(rec_mount(d, &v) == 0);
    CHECK(v->generation == gen);
    CHECK(read_eq(&v->live, "keep", "committed"));
    CHECK(fs_lookup(&v->live, "lost", NULL) == -1);
    CowTree t;
    CHECK(ckpt_read(v, "cp", &t) == 0 && read_eq(&t, "keep", "committed"));
    CHECK(vol_check(v) == 0);

    uint8_t junk[BLOCK_SIZE];                       /* corrupt primary superblock */
    memset(junk, 0xAB, sizeof junk);
    disk_write(d, SB_PRIMARY, junk);
    vol_free(v);
    CHECK(rec_mount(d, &v) == 1);                   /* mirror used */
    CHECK(read_eq(&v->live, "keep", "committed"));
    rec_commit(v);                                  /* repairs the primary */
    vol_free(v);
    CHECK(rec_mount(d, &v) == 0);
    vol_free(v); disk_destroy(d);

    const char *path = "build/test_disk.img";       /* real file, real restart */
    remove(path);
    int existed;
    d = filedisk_open(path, &existed);
    CHECK(!existed);
    v = rec_format(d);
    fs_write(v, "persist", "on disk", 7);
    rec_commit(v);
    vol_free(v); disk_destroy(d);
    d = filedisk_open(path, &existed);
    CHECK(existed && rec_mount(d, &v) == 0);
    CHECK(read_eq(&v->live, "persist", "on disk"));
    vol_free(v); disk_destroy(d);
    remove(path);

    Disk *blank = ramdisk_create();
    CHECK(rec_mount(blank, &v) == -1);
    disk_destroy(blank);
}

int main(void)
{
    test_files();
    test_checkpoints();
    test_duplication();
    test_recovery();
    if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
    puts("all tests passed");
    return 0;
}
