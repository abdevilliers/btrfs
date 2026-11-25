/*
 * print_tree.c - btrfs inspect-internal dump-tree and dump-super, in the
 * same shape as btrfs-progs prints them.
 */
#include <stdio.h>

#include "ctree.h"

static const char *type_name(uint8_t t)
{
    switch (t) {
    case BTRFS_INODE_ITEM_KEY: return "INODE_ITEM";
    case BTRFS_INODE_REF_KEY: return "INODE_REF";
    case BTRFS_ORPHAN_ITEM_KEY: return "ORPHAN_ITEM";
    case BTRFS_DIR_ITEM_KEY: return "DIR_ITEM";
    case BTRFS_DIR_INDEX_KEY: return "DIR_INDEX";
    case BTRFS_EXTENT_DATA_KEY: return "EXTENT_DATA";
    case BTRFS_EXTENT_CSUM_KEY: return "EXTENT_CSUM";
    case BTRFS_ROOT_ITEM_KEY: return "ROOT_ITEM";
    case BTRFS_ROOT_BACKREF_KEY: return "ROOT_BACKREF";
    case BTRFS_ROOT_REF_KEY: return "ROOT_REF";
    case BTRFS_EXTENT_ITEM_KEY: return "EXTENT_ITEM";
    case BTRFS_METADATA_ITEM_KEY: return "METADATA_ITEM";
    case BTRFS_TREE_BLOCK_REF_KEY: return "TREE_BLOCK_REF";
    case BTRFS_EXTENT_DATA_REF_KEY: return "EXTENT_DATA_REF";
    case BTRFS_SHARED_BLOCK_REF_KEY: return "SHARED_BLOCK_REF";
    case BTRFS_SHARED_DATA_REF_KEY: return "SHARED_DATA_REF";
    case BTRFS_BLOCK_GROUP_ITEM_KEY: return "BLOCK_GROUP_ITEM";
    case BTRFS_FREE_SPACE_INFO_KEY: return "FREE_SPACE_INFO";
    case BTRFS_FREE_SPACE_EXTENT_KEY: return "FREE_SPACE_EXTENT";
    case BTRFS_UUID_KEY_SUBVOL: return "UUID_KEY_SUBVOL";
    default: return "UNKNOWN";
    }
}

static void print_objectid(uint64_t id)
{
    switch (id) {
    case BTRFS_ROOT_TREE_OBJECTID: printf("ROOT_TREE"); return;
    case BTRFS_EXTENT_TREE_OBJECTID: printf("EXTENT_TREE"); return;
    case BTRFS_FS_TREE_OBJECTID: printf("FS_TREE"); return;
    case BTRFS_ROOT_TREE_DIR_OBJECTID: printf("ROOT_TREE_DIR"); return;
    case BTRFS_CSUM_TREE_OBJECTID: printf("CSUM_TREE"); return;
    case BTRFS_UUID_TREE_OBJECTID: printf("UUID_TREE"); return;
    case BTRFS_FREE_SPACE_TREE_OBJECTID: printf("FREE_SPACE_TREE"); return;
    case BTRFS_ORPHAN_OBJECTID: printf("ORPHAN"); return;
    case BTRFS_EXTENT_CSUM_OBJECTID: printf("EXTENT_CSUM"); return;
    default: printf("%llu", (unsigned long long)id);
    }
}

static void print_key(const BtrfsKey *k)
{
    printf("(");
    print_objectid(k->objectid);
    printf(" %s ", type_name(k->type));
    if (k->offset == (uint64_t)-1)
        printf("-1)");
    else
        printf("%llu)", (unsigned long long)k->offset);
}

static void print_names(const uint8_t *d, uint32_t size, int dir)
{
    for (uint32_t off = 0; off < size;) {
        if (dir) {
            BtrfsDirItem di;
            memcpy(&di, d + off, sizeof di);
            BtrfsKey loc = { di.location.objectid, di.location.type, di.location.offset };
            printf("\t\tlocation key ");
            print_key(&loc);
            printf(" type %s\n\t\ttransid %llu data_len %u name_len %u\n\t\tname: %.*s\n",
                   di.type == BTRFS_FT_DIR ? "DIR" : "FILE", (unsigned long long)di.transid, di.data_len,
                   di.name_len, di.name_len, (const char *)d + off + sizeof di);
            off += (uint32_t)sizeof di + di.name_len + di.data_len;
        } else {
            BtrfsInodeRef ir;
            memcpy(&ir, d + off, sizeof ir);
            printf("\t\tindex %llu namelen %u name: %.*s\n", (unsigned long long)ir.index, ir.name_len,
                   ir.name_len, (const char *)d + off + sizeof ir);
            off += (uint32_t)sizeof ir + ir.name_len;
        }
    }
}

static void print_uuid(const uint8_t *u)
{
    for (int i = 0; i < 16; i++)
        printf("%02x%s", u[i], i == 3 || i == 5 || i == 7 || i == 9 ? "-" : "");
}

static void print_item(const BtrfsKey *k, const uint8_t *d, uint32_t size)
{
    switch (k->type) {
    case BTRFS_INODE_ITEM_KEY: {
        BtrfsInodeItem ii;
        memcpy(&ii, d, sizeof ii);
        printf("\t\tgeneration %llu transid %llu size %llu nbytes %llu\n\t\tnlink %u mode %o\n",
               (unsigned long long)ii.generation, (unsigned long long)ii.transid, (unsigned long long)ii.size,
               (unsigned long long)ii.nbytes, ii.nlink, ii.mode);
        break;
    }
    case BTRFS_INODE_REF_KEY:
        print_names(d, size, 0);
        break;
    case BTRFS_DIR_ITEM_KEY:
    case BTRFS_DIR_INDEX_KEY:
        print_names(d, size, 1);
        break;
    case BTRFS_EXTENT_DATA_KEY: {
        BtrfsFileExtentItem fi;
        memset(&fi, 0, sizeof fi);
        memcpy(&fi, d, size < sizeof fi ? size : sizeof fi);
        printf("\t\tgeneration %llu type %u (%s)\n", (unsigned long long)fi.generation, fi.type,
               fi.type == BTRFS_FILE_EXTENT_INLINE ? "inline" : "regular");
        if (fi.type == BTRFS_FILE_EXTENT_INLINE)
            printf("\t\tinline extent data size %u ram_bytes %llu\n", size - BTRFS_FILE_EXTENT_INLINE_DATA_START,
                   (unsigned long long)fi.ram_bytes);
        else
            printf("\t\textent data disk byte %llu nr %llu\n\t\textent data offset %llu nr %llu ram %llu\n",
                   (unsigned long long)fi.disk_bytenr, (unsigned long long)fi.disk_num_bytes,
                   (unsigned long long)fi.offset, (unsigned long long)fi.num_bytes, (unsigned long long)fi.ram_bytes);
        break;
    }
    case BTRFS_EXTENT_CSUM_KEY:
        printf("\t\trange start %llu end %llu length %llu\n", (unsigned long long)k->offset,
               (unsigned long long)(k->offset + size / 4 * SECTORSIZE), (unsigned long long)(size / 4 * SECTORSIZE));
        break;
    case BTRFS_ROOT_ITEM_KEY: {
        BtrfsRootItem ri;
        memset(&ri, 0, sizeof ri);
        memcpy(&ri, d, size < sizeof ri ? size : sizeof ri);
        printf("\t\tgeneration %llu root_dirid %llu bytenr %llu level %u refs %u\n"
               "\t\tlastsnap %llu byte_limit %llu bytes_used %llu flags 0x%llx(%s)\n\t\tuuid ",
               (unsigned long long)ri.generation, (unsigned long long)ri.root_dirid, (unsigned long long)ri.bytenr,
               ri.level, ri.refs, (unsigned long long)ri.last_snapshot, (unsigned long long)ri.byte_limit,
               (unsigned long long)ri.bytes_used, (unsigned long long)ri.flags,
               ri.flags & BTRFS_ROOT_SUBVOL_RDONLY ? "RDONLY" : "none");
        print_uuid(ri.uuid);
        printf("\n\t\tparent_uuid ");
        print_uuid(ri.parent_uuid);
        printf("\n\t\tctransid %llu otransid %llu\n", (unsigned long long)ri.ctransid, (unsigned long long)ri.otransid);
        break;
    }
    case BTRFS_ROOT_REF_KEY:
    case BTRFS_ROOT_BACKREF_KEY: {
        BtrfsRootRef rr;
        memcpy(&rr, d, sizeof rr);
        printf("\t\t%s dirid %llu sequence %llu name %.*s\n", k->type == BTRFS_ROOT_REF_KEY ? "root ref" : "root backref",
               (unsigned long long)rr.dirid, (unsigned long long)rr.sequence, rr.name_len, (const char *)d + sizeof rr);
        break;
    }
    case BTRFS_EXTENT_ITEM_KEY:
    case BTRFS_METADATA_ITEM_KEY: {
        BtrfsExtentItem ei;
        memcpy(&ei, d, sizeof ei);
        printf("\t\trefs %llu gen %llu flags %s%s\n", (unsigned long long)ei.refs, (unsigned long long)ei.generation,
               ei.flags & BTRFS_EXTENT_FLAG_DATA ? "DATA" : "TREE_BLOCK",
               ei.flags & BTRFS_BLOCK_FLAG_FULL_BACKREF ? "|FULL_BACKREF" : "");
        break;
    }
    case BTRFS_EXTENT_DATA_REF_KEY: {
        BtrfsExtentDataRef dr;
        memcpy(&dr, d, sizeof dr);
        printf("\t\textent data backref root %llu objectid %llu offset %llu count %u\n", (unsigned long long)dr.root,
               (unsigned long long)dr.objectid, (unsigned long long)dr.offset, dr.count);
        break;
    }
    case BTRFS_SHARED_DATA_REF_KEY: {
        BtrfsSharedDataRef sr;
        memcpy(&sr, d, sizeof sr);
        printf("\t\tshared data backref parent %llu count %u\n", (unsigned long long)k->offset, sr.count);
        break;
    }
    case BTRFS_TREE_BLOCK_REF_KEY:
        printf("\t\ttree block backref root ");
        print_objectid(k->offset);
        printf("\n");
        break;
    case BTRFS_SHARED_BLOCK_REF_KEY:
        printf("\t\tshared block backref parent %llu\n", (unsigned long long)k->offset);
        break;
    case BTRFS_BLOCK_GROUP_ITEM_KEY: {
        BtrfsBlockGroupItem bi;
        memcpy(&bi, d, sizeof bi);
        printf("\t\tblock group used %llu chunk_objectid %llu flags %s\n", (unsigned long long)bi.used,
               (unsigned long long)bi.chunk_objectid, bi.flags & BTRFS_BLOCK_GROUP_DATA ? "DATA" : "METADATA");
        break;
    }
    case BTRFS_FREE_SPACE_INFO_KEY: {
        BtrfsFreeSpaceInfo fi;
        memcpy(&fi, d, sizeof fi);
        printf("\t\tfree space info extent count %u flags %u\n", fi.extent_count, fi.flags);
        break;
    }
    case BTRFS_UUID_KEY_SUBVOL:
        for (uint32_t off = 0; off + 8 <= size; off += 8)
            printf("\t\tsubvol_id %llu\n", (unsigned long long)get64(d + off));
        break;
    default:
        break;
    }
}

static void dump_block(Btrfs *b, ExtentBuffer *eb)
{
    uint32_t n = eb_nritems(eb);
    int level = eb_level(eb);
    printf("%s %llu level %d items %u free space %u generation %llu owner ", level ? "node" : "leaf",
           (unsigned long long)eb->start, level, n,
           level ? (max_node_ptrs(b) - n) * KEYPTR_SIZE : leaf_free_space(b, eb), (unsigned long long)eb_gen(eb));
    print_objectid(eb_owner(eb));
    printf("\n");
    for (uint32_t i = 0; i < n; i++) {
        BtrfsKey k;
        if (level) {
            node_key(eb, (int)i, &k);
            printf("\tkey ");
            print_key(&k);
            printf(" block %llu gen %llu\n", (unsigned long long)node_blockptr(eb, (int)i),
                   (unsigned long long)node_ptr_gen(eb, (int)i));
        } else {
            item_key(eb, (int)i, &k);
            printf("\titem %u key ", i);
            print_key(&k);
            printf(" itemoff %u itemsize %u\n", item_offset(eb, (int)i), item_size(eb, (int)i));
            print_item(&k, item_ptr(eb, (int)i), item_size(eb, (int)i));
        }
    }
    if (level)
        for (uint32_t i = 0; i < n; i++) {
            ExtentBuffer *c;
            if (read_tree_block(b, node_blockptr(eb, (int)i), node_ptr_gen(eb, (int)i), level - 1, &c) == 0)
                dump_block(b, c);
        }
}

int btrfs_dump_tree(Btrfs *b, uint64_t tree_id)
{
    Root *r = tree_id == BTRFS_ROOT_TREE_OBJECTID ? b->tree_root : get_root(b, tree_id);
    if (!r)
        return -1;
    dump_block(b, r->node);
    return 0;
}

void btrfs_dump_super(Btrfs *b)
{
    const BtrfsSuper *s = &b->super;
    printf("superblock: bytenr=%llu (copies at 65536, 67108864 if the disk is big enough)\n",
           (unsigned long long)BTRFS_SUPER_INFO_OFFSET);
    printf("magic\t\t\t_BHRfS_M\nfsid\t\t\t");
    print_uuid(s->fsid);
    printf("\nlabel\t\t\t%s\ngeneration\t\t%llu\nroot\t\t\t%llu\nroot_level\t\t%u\n", s->label,
           (unsigned long long)s->generation, (unsigned long long)s->root, s->root_level);
    printf("total_bytes\t\t%llu\nbytes_used\t\t%llu\nsectorsize\t\t%u\nnodesize\t\t%u\n",
           (unsigned long long)s->total_bytes, (unsigned long long)s->bytes_used, s->sectorsize, s->nodesize);
    printf("incompat_flags\t\t0x%llx ( MIXED_BACKREF | SKINNY_METADATA | NO_HOLES )\n",
           (unsigned long long)s->incompat_flags);
    printf("compat_ro_flags\t\t0x%llx ( FREE_SPACE_TREE | FREE_SPACE_TREE_VALID )\n",
           (unsigned long long)s->compat_ro_flags);
    printf("backup_roots:\n");
    for (int i = 0; i < BTRFS_NUM_BACKUP_ROOTS; i++) {
        const BtrfsRootBackup *bk = &s->super_roots[i];
        printf("\tbackup %d: tree root %llu gen %llu, extent root %llu, fs root %llu, csum root %llu\n", i,
               (unsigned long long)bk->tree_root, (unsigned long long)bk->tree_root_gen,
               (unsigned long long)bk->extent_root, (unsigned long long)bk->fs_root, (unsigned long long)bk->csum_root);
    }
}
