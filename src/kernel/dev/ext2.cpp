#include "kernel/dev/ext2.h"
#include "kernel/core/block.h"
#include "fs/vfs.h"

extern "C" void kprint(const char* str);
extern "C" void kprint_uint(uint64_t value);

// --- ext2, read-only ---
//
// The writer is someone else's (mke2fs on the host); this is a reader, and a
// reader can be small. What it has to get right is the shape: the superblock at
// byte 1024, group descriptors after it, an inode table per group, and directory
// entries that are just variable-length records with a name in them. Blocks are
// reached from the inode's fifteen pointers — twelve direct, then single, then
// double indirect — which is why a file bigger than 12 blocks is the interesting
// test case.
//
// It is read-only by construction: there is no writer here and no code that
// could grow the image, so a `write` on this mount fails at the VFS (EROFS).
//
// Not modelled: symlinks, special files, extended attributes, extents, 64-bit
// filesystems. Each is refused or read as best it can be, and none of them is
// something the image we make actually contains.

namespace Ext2 {

constexpr uint16_t MAGIC = 0xEF53;
constexpr uint32_t INCOMPAT_64BIT = 0x0080;
constexpr uint32_t S_IFDIR_BIT = 0040000;

struct Superblock {
    uint32_t inodes_count;        // 0
    uint32_t blocks_count;        // 4
    uint32_t r_blocks_count;      // 8
    uint32_t free_blocks_count;   // 12
    uint32_t free_inodes_count;   // 16
    uint32_t first_data_block;    // 20
    uint32_t log_block_size;      // 24
    uint32_t log_frag_size;       // 28
    uint32_t blocks_per_group;    // 32
    uint32_t frags_per_group;     // 36
    uint32_t inodes_per_group;    // 40
    uint32_t mtime;               // 44
    uint32_t wtime;               // 48
    uint16_t mnt_count;           // 52
    uint16_t max_mnt_count;       // 54
    uint16_t magic;               // 56
    uint16_t state;               // 58
    uint16_t errors;              // 60
    uint16_t minor_rev_level;     // 62
    uint32_t lastcheck;           // 64
    uint32_t checkinterval;       // 68
    uint32_t creator_os;          // 72
    uint32_t rev_level;           // 76
    uint16_t def_resuid;          // 80
    uint16_t def_resgid;          // 82
    uint32_t first_ino;           // 84
    uint16_t inode_size;          // 88
    uint16_t block_group_nr;      // 90
    uint32_t feature_compat;      // 92
    uint32_t feature_incompat;    // 96
    uint32_t feature_ro_compat;   // 100
} __attribute__((packed));
static_assert(sizeof(Superblock) == 104, "ext2 superblock prefix");

struct GroupDesc {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint8_t  reserved[12];
} __attribute__((packed));
static_assert(sizeof(GroupDesc) == 32, "ext2 group descriptor");

struct Inode {
    uint16_t mode;
    uint16_t uid;
    uint32_t size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks;
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[15];       // 12 direct, then single/double/triple indirect
    uint32_t generation;
    uint32_t file_acl;
    uint32_t dir_acl;
    uint32_t faddr;
    uint32_t osd2[3];
} __attribute__((packed));
static_assert(sizeof(Inode) == 128, "ext2 inode prefix");

struct RawDirEntry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
} __attribute__((packed));
static_assert(sizeof(RawDirEntry) == 8, "ext2 directory entry header");

// --- the mount ---

struct Fs {
    Block::Device* dev;
    uint32_t block_size;
    uint32_t first_data_block;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t inodes_count;
    uint32_t groups;
};

// A node: the inode's identity and metadata, plus its block pointers, so that
// reading a file does not re-read its inode for every block. Nothing can move
// them — the mount is read-only.
struct E2Node {
    uint32_t ino;
    bool     is_dir;
    uint64_t size;
    uint32_t mode;
    uint64_t mtime;
    uint32_t block[15];
};

// A block on the stack; ext2 block sizes run 1K..4K and 4K is the ceiling we
// accept at mount time, so one block always fits.
constexpr uint32_t MAX_BLOCK = 4096;

// --- the ops (prototyped so the table below can name them) ---
static VFS::Node* e2_root(void* priv);
static VFS::Node* e2_lookup(void* priv, VFS::Node* dir, const char* name);
static bool e2_readdir(void* priv, VFS::Node* dir, uint64_t index, VFS::DirEntry* out);
static uint64_t e2_read(void* priv, VFS::Node* node, void* buf,
                        uint64_t count, uint64_t offset);
static void e2_release(void* priv, VFS::Node* node);

static VFS::Filesystem inst = {
    .fs_type  = "ext2",
    .priv     = nullptr,
    .root     = e2_root,
    .lookup   = e2_lookup,
    .readdir  = e2_readdir,
    .read      = e2_read,
    .write     = nullptr,   // read-only
    .truncate = nullptr,
    .create   = nullptr,
    .unlink   = nullptr,
    .rename   = nullptr,
    .release  = e2_release,
    .map      = nullptr,    // the RAM disk could, but nothing promised it
};

// --- reading ---
// The transfer unit is the sector, so this is the one place partial-sector
// reads are assembled; everything above it names bytes and blocks.
static bool read_bytes(Fs* f, uint64_t off, void* dst, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    uint8_t sec[Block::SECTOR];
    while (n > 0) {
        uint64_t lba = off / Block::SECTOR;
        uint32_t boff = (uint32_t)(off % Block::SECTOR);
        uint32_t chunk = Block::SECTOR - boff;
        if (chunk > n) chunk = n;
        if (f->dev->read(f->dev->priv, lba, 1, sec) != 1) return false;
        for (uint32_t i = 0; i < chunk; i++) d[i] = sec[boff + i];
        d += chunk;
        off += chunk;
        n -= chunk;
    }
    return true;
}

// The physical block holding file block 'idx' of an inode, or 0 for a hole.
static uint32_t map_block(Fs* f, const uint32_t* bp, uint32_t idx) {
    uint32_t buf[MAX_BLOCK / 4];
    const uint32_t ptrs = f->block_size / 4;

    if (idx < 12) return bp[idx];
    idx -= 12;

    if (idx < ptrs) {
        if (bp[12] == 0) return 0;
        if (!read_bytes(f, (uint64_t)bp[12] * f->block_size, buf, f->block_size))
            return 0;
        return buf[idx];
    }
    idx -= ptrs;

    if (idx < ptrs * ptrs) {
        if (bp[13] == 0) return 0;
        if (!read_bytes(f, (uint64_t)bp[13] * f->block_size, buf, f->block_size))
            return 0;
        uint32_t mid = buf[idx / ptrs];
        if (mid == 0) return 0;
        if (!read_bytes(f, (uint64_t)mid * f->block_size, buf, f->block_size))
            return 0;
        return buf[idx % ptrs];
    }
    return 0;   // triple indirect: beyond anything this mount is asked for
}

static bool read_inode(Fs* f, uint32_t ino, Inode* out) {
    if (ino == 0 || ino > f->inodes_count) return false;
    uint32_t group = (ino - 1) / f->inodes_per_group;
    uint32_t index = (ino - 1) % f->inodes_per_group;
    if (group >= f->groups) return false;

    GroupDesc gd;
    uint64_t gd_off = ((uint64_t)f->first_data_block + 1) * f->block_size
                    + (uint64_t)group * sizeof(GroupDesc);
    if (!read_bytes(f, gd_off, &gd, sizeof(gd))) return false;

    uint64_t off = (uint64_t)gd.inode_table * f->block_size
                 + (uint64_t)index * f->inode_size;
    return read_bytes(f, off, out, sizeof(Inode));
}

static VFS::Node* make_node(Fs* f, uint32_t ino) {
    Inode i;
    if (!read_inode(f, ino, &i)) return nullptr;

    E2Node* n = new E2Node;
    if (n == nullptr) return nullptr;
    n->ino = ino;
    n->is_dir = (i.mode & S_IFDIR_BIT) != 0;
    n->size = i.size;
    n->mode = i.mode & 0777;
    n->mtime = i.mtime;
    for (int k = 0; k < 15; k++) n->block[k] = i.block[k];

    VFS::Node* v = VFS::node_new(&inst, n->is_dir ? VFS::FT_DIR : VFS::FT_FILE);
    if (v == nullptr) { delete n; return nullptr; }
    v->priv = n;
    v->size = n->size;
    v->ino = n->ino;
    v->mode = n->mode;
    v->mtime = n->mtime;
    return v;
}

// Walk a directory's data blocks, handing each record to 'visit'. The walker is
// the common part of lookup and readdir; both need the same linear pass.
template <typename F>
static bool walk_dir(Fs* f, E2Node* d, F visit) {
    uint32_t buf[MAX_BLOCK / 4];
    uint32_t nblocks = (uint32_t)((d->size + f->block_size - 1) / f->block_size);

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = map_block(f, d->block, b);
        if (blk == 0) continue;
        if (!read_bytes(f, (uint64_t)blk * f->block_size, buf, f->block_size))
            return false;

        uint8_t* raw = (uint8_t*)buf;
        uint32_t p = 0;
        while (p + sizeof(RawDirEntry) <= f->block_size) {
            RawDirEntry* de = (RawDirEntry*)(raw + p);
            if (de->rec_len < sizeof(RawDirEntry) ||
                p + de->rec_len > f->block_size)
                break;   // malformed: stop rather than walk off the block
            const char* name = (const char*)(raw + p + sizeof(RawDirEntry));
            uint32_t nlen = de->name_len;
            // "." and ".." are real records on disk, but nothing above the VFS
            // wants them: the node layer does not model them, and the filer
            // synthesises its own "..".
            bool dot = (nlen == 1 && name[0] == '.') ||
                       (nlen == 2 && name[0] == '.' && name[1] == '.');
            if (de->inode != 0 && nlen > 0 && !dot &&
                nlen <= de->rec_len - sizeof(RawDirEntry)) {
                if (!visit(name, nlen, de->inode)) return true;   // visitor is done
            }
            p += de->rec_len;
        }
    }
    return true;
}

// --- the ops ---

static VFS::Node* e2_root(void* priv) {
    return make_node((Fs*)priv, 2);   // ext2's root inode is fixed at 2
}

static VFS::Node* e2_lookup(void* priv, VFS::Node* dir, const char* name) {
    Fs* f = (Fs*)priv;
    E2Node* d = (E2Node*)dir->priv;
    if (!d->is_dir) return nullptr;

    uint32_t want = 0;
    while (name[want]) want++;
    if (want == 0 || want > 255) return nullptr;

    VFS::Node* found = nullptr;
    walk_dir(f, d, [&](const char* n, uint8_t len, uint32_t ino) {
        if (len != want) return true;
        for (uint32_t k = 0; k < want; k++)
            if (n[k] != name[k]) return true;
        found = make_node(f, ino);
        return false;
    });
    return found;
}

static bool e2_readdir(void* priv, VFS::Node* dir, uint64_t index, VFS::DirEntry* out) {
    Fs* f = (Fs*)priv;
    E2Node* d = (E2Node*)dir->priv;
    if (!d->is_dir) return false;

    uint64_t seen = 0;
    bool found = false;
    walk_dir(f, d, [&](const char* n, uint8_t len, uint32_t ino) {
        if (seen == index) {
            uint32_t k = 0;
            while (k < len && k + 1 < sizeof(out->name)) { out->name[k] = n[k]; k++; }
            out->name[k] = '\0';
            // The record's file_type is authoritative when the filesystem has
            // the feature; the inode is the answer when it does not.
            Inode i;
            bool is_dir = read_inode(f, ino, &i) && (i.mode & S_IFDIR_BIT) != 0;
            out->type = is_dir ? VFS::FT_DIR : VFS::FT_FILE;
            out->ino = ino;
            found = true;
            return false;
        }
        seen++;
        return true;
    });
    return found;
}

static uint64_t e2_read(void* priv, VFS::Node* node, void* buf,
                        uint64_t count, uint64_t offset) {
    Fs* f = (Fs*)priv;
    E2Node* n = (E2Node*)node->priv;
    if (n->is_dir) return 0;
    if (offset >= n->size) return 0;
    if (count > n->size - offset) count = n->size - offset;

    uint8_t* dst = (uint8_t*)buf;
    uint64_t done = 0;
    while (done < count) {
        uint64_t pos = offset + done;
        uint32_t idx = (uint32_t)(pos / f->block_size);
        uint32_t boff = (uint32_t)(pos % f->block_size);
        uint32_t chunk = f->block_size - boff;
        if (chunk > count - done) chunk = (uint32_t)(count - done);

        uint32_t blk = map_block(f, n->block, idx);
        if (blk == 0) break;   // a hole contributes nothing
        if (!read_bytes(f, (uint64_t)blk * f->block_size + boff, dst + done, chunk))
            break;
        done += chunk;
    }
    return done;
}

static void e2_release(void* priv, VFS::Node* node) {
    (void)priv;
    delete (E2Node*)node->priv;
    delete node;
}

// --- mount ---

VFS::Filesystem* mount(Block::Device* dev) {
    if (dev == nullptr) return nullptr;

    Fs* f = new Fs;
    if (f == nullptr) return nullptr;
    f->dev = dev;
    f->block_size = 0;
    f->first_data_block = 0;
    f->blocks_per_group = 0;
    f->inodes_per_group = 0;
    f->inode_size = 0;
    f->inodes_count = 0;
    f->groups = 0;

    // The superblock is always at byte 1024 — before the block size is known,
    // so this read cannot go through the block helpers.
    Superblock sb;
    if (!read_bytes(f, 1024, &sb, sizeof(sb)) || sb.magic != MAGIC) {
        delete f;
        return nullptr;
    }
    if (sb.rev_level >= 1 && (sb.feature_incompat & INCOMPAT_64BIT)) {
        delete f;
        return nullptr;
    }

    uint32_t bs = 1024u << sb.log_block_size;
    if (bs < 512 || bs > MAX_BLOCK || sb.blocks_per_group == 0 ||
        sb.inodes_per_group == 0 || sb.blocks_count == 0) {
        delete f;
        return nullptr;
    }

    f->block_size = bs;
    f->first_data_block = sb.first_data_block;
    f->blocks_per_group = sb.blocks_per_group;
    f->inodes_per_group = sb.inodes_per_group;
    f->inodes_count = sb.inodes_count;
    f->inode_size = (sb.rev_level >= 1 && sb.inode_size >= sizeof(Inode))
                        ? sb.inode_size : sizeof(Inode);
    f->groups = (sb.blocks_count + sb.blocks_per_group - 1) / sb.blocks_per_group;

    inst.priv = f;

    kprint("[Ext2] ");
    kprint_uint(bs);
    kprint("-byte blocks, ");
    kprint_uint(sb.blocks_count);
    kprint(" blocks, ");
    kprint_uint(sb.inodes_count);
    kprint(" inodes, ");
    kprint_uint(f->groups);
    kprint(f->groups == 1 ? " group (read-only)\n" : " groups (read-only)\n");
    return &inst;
}

} // namespace Ext2
