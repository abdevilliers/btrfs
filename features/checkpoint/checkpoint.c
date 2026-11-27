#include "checkpoint.h"
#include <string.h>

static int find(Volume *v, const char *name)
{
    for (int i = 0; i < MAX_CKPT; i++)
        if (v->table[i].used && strncmp(v->table[i].name, name, CKPT_NAME) == 0)
            return i;
    return -1;
}

int ckpt_create(Volume *v, const char *name)
{
    if (!name[0] || strlen(name) >= CKPT_NAME || find(v, name) >= 0) return -1;
    for (int i = 0; i < MAX_CKPT; i++) {
        RootEntry *e = &v->table[i];
        if (e->used) continue;
        memset(e, 0, sizeof *e);
        e->used = 1;
        e->id = v->next_id++;
        e->root = v->live.root;
        e->generation = v->generation;
        strncpy(e->name, name, CKPT_NAME - 1);
        if (e->root) alloc_ref(v->alloc, e->root);   /* live root refs +1 */
        return 0;
    }
    return -1;                                        /* table full */
}

int ckpt_delete(Volume *v, const char *name)
{
    int i = find(v, name);
    if (i < 0) return -1;
    cowtree_drop(v->disk, v->alloc, v->table[i].root, DEPTH); /* stops at shared */
    memset(&v->table[i], 0, sizeof v->table[i]);
    return 0;
}

int ckpt_list(Volume *v, RootEntry out[MAX_CKPT])
{
    int n = 0;
    for (int i = 0; i < MAX_CKPT; i++)
        if (v->table[i].used) out[n++] = v->table[i];
    for (int i = 1; i < n; i++) {                     /* newest first */
        RootEntry x = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].id < x.id) { out[j + 1] = out[j]; j--; }
        out[j + 1] = x;
    }
    return n;
}

int ckpt_read(Volume *v, const char *name, CowTree *out)
{
    int i = find(v, name);
    if (i < 0) return -1;
    *out = vol_tree(v, v->table[i].root);
    return 0;
}

int ckpt_rollback(Volume *v, const char *name, int keep_newer)
{
    int i = find(v, name);
    if (i < 0) return -1;
    uint32_t target = v->table[i].root, id = v->table[i].id;

    if (target) alloc_ref(v->alloc, target);          /* live gets a ref */
    cowtree_drop(v->disk, v->alloc, v->live.root, DEPTH); /* old live tree */
    v->live.root = target;

    if (!keep_newer) {
        RootEntry list[MAX_CKPT];
        int n = ckpt_list(v, list);
        for (int k = 0; k < n; k++)
            if (list[k].id > id) ckpt_delete(v, list[k].name);
    }
    return 0;
}

int ckpt_rollback_back(Volume *v, int n_back, int keep_newer)
{
    RootEntry list[MAX_CKPT];
    int n = ckpt_list(v, list);
    if (n_back < 1 || n_back > n) return -1;
    char name[CKPT_NAME];
    strcpy(name, list[n_back - 1].name);
    return ckpt_rollback(v, name, keep_newer);
}
