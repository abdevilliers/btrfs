#include "fs.h"
#include <string.h>

typedef struct { Inode inode[BLOCK_SIZE / sizeof(Inode)]; } InodeTable;

uint32_t fs_data_page(int slot, int k) { return 1u + (uint32_t)(slot * PAGES_PER_FILE + k); }

static void table_read(const CowTree *t, InodeTable *it) { vol_read_page(t, 0, it); }

static int table_find(const InodeTable *it, const char *name)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (it->inode[i].used && strncmp(it->inode[i].name, name, NAME_LEN) == 0)
            return i;
    return -1;
}

int fs_lookup(const CowTree *t, const char *name, Inode *out)
{
    InodeTable it;
    table_read(t, &it);
    int i = table_find(&it, name);
    if (i >= 0 && out) *out = it.inode[i];
    return i;
}

int fs_list(const CowTree *t, Inode out[MAX_FILES])
{
    InodeTable it;
    table_read(t, &it);
    int n = 0;
    for (int i = 0; i < MAX_FILES; i++)
        if (it.inode[i].used) out[n++] = it.inode[i];
    return n;
}

long fs_read(const CowTree *t, const char *name, void *buf, uint32_t cap)
{
    Inode ino;
    int slot = fs_lookup(t, name, &ino);
    if (slot < 0) return -1;
    uint8_t page[BLOCK_SIZE];
    uint32_t done = 0;
    for (int k = 0; done < ino.size && done < cap; k++) {
        vol_read_page(t, fs_data_page(slot, k), page);
        uint32_t n = ino.size - done;
        if (n > BLOCK_SIZE) n = BLOCK_SIZE;
        if (n > cap - done) n = cap - done;
        memcpy((uint8_t *)buf + done, page, n);
        done += n;
    }
    return (long)done;
}

int fs_create(Volume *v, const char *name)
{
    if (strlen(name) == 0 || strlen(name) >= NAME_LEN) return -1;
    InodeTable it;
    table_read(&v->live, &it);
    int i = table_find(&it, name);
    if (i >= 0) return i;
    for (i = 0; i < MAX_FILES; i++) {
        if (!it.inode[i].used) {
            memset(&it.inode[i], 0, sizeof(Inode));
            it.inode[i].used = 1;
            it.inode[i].ino = FIRST_INO + (uint32_t)i;
            strncpy(it.inode[i].name, name, NAME_LEN - 1);
            vol_write_page(v, 0, &it);
            return i;
        }
    }
    return -1;
}

void fs_set_size(Volume *v, int slot, uint32_t size)
{
    InodeTable it;
    table_read(&v->live, &it);
    if (it.inode[slot].size == size) return;
    it.inode[slot].size = size;
    vol_write_page(v, 0, &it);
}

int fs_write(Volume *v, const char *name, const void *data, uint32_t len)
{
    if (len > MAX_FILE_SIZE) return -1;
    int slot = fs_create(v, name);
    if (slot < 0) return -1;

    int written = 0;
    uint8_t newp[BLOCK_SIZE], oldp[BLOCK_SIZE];
    for (int k = 0; k < PAGES_PER_FILE; k++) {
        uint32_t page = fs_data_page(slot, k);
        uint32_t off = (uint32_t)k * BLOCK_SIZE;
        uint32_t old = cowtree_get(&v->live, page);
        if (off < len) {
            uint32_t n = len - off < BLOCK_SIZE ? len - off : BLOCK_SIZE;
            memset(newp, 0, BLOCK_SIZE);
            memcpy(newp, (const uint8_t *)data + off, n);
            if (old) {
                disk_read(v->disk, old, oldp);
                if (memcmp(oldp, newp, BLOCK_SIZE) == 0) continue;  /* unchanged */
            }
            vol_write_page(v, page, newp);
            written++;
        } else if (old) {
            cowtree_set(&v->live, page, 0);   /* file got shorter */
        }
    }
    fs_set_size(v, slot, len);
    return written;
}

int fs_delete(Volume *v, const char *name)
{
    InodeTable it;
    table_read(&v->live, &it);
    int slot = table_find(&it, name);
    if (slot < 0) return -1;
    for (int k = 0; k < PAGES_PER_FILE; k++)
        if (cowtree_get(&v->live, fs_data_page(slot, k)))
            cowtree_set(&v->live, fs_data_page(slot, k), 0);
    memset(&it.inode[slot], 0, sizeof(Inode));
    vol_write_page(v, 0, &it);
    return 0;
}
