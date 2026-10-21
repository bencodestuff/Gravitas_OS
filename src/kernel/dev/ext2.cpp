#include "kernel/dev/ext2.h"
#include "kernel/core/block.h"
#include "kernel/klib/klib.h"
#include "kernel/dev/rtc.h"
#include "fs/vfs.h"

extern "C" void kprint(const char* str);
extern "C" void kprint_uint(uint64_t value);

// --- ext2, read and write ---
//
// The writer is someone else's (mke2fs on the host); this is a reader that can
// also write in place. What it has to get right is the shape: the superblock at
// byte 1024, group descriptors after it, an inode table per group, and directory
// entries that are just variable-length records with a name in them. Blocks are
// reached from the inode's fifteen pointers — twelve direct, then single, then
// double indirect — which is why a file bigger than 12 blocks is the interesting
// test case.
//
// The read path is the one the mount started with: it trusts the image and
// re-reads nothing it does not need. The write path is deliberately more
// careful, because after the first write the image is ours: every mutation takes
// the device's lock with interrupts off, re-reads the inode it is about to
// change rather than trusting a cached copy, and puts the inode and bitmap
// changes back before it returns. A write only ever touches the standard
// 128-byte inode prefix, so the 256-byte inodes this image was made with keep
// their extra fields (birth time and friends).
//
// Not modelled: symlinks, special files, extended attributes, extents, 64-bit
// filesystems. Each is refused or read as best it can be, and none of them is
// something the image we make actually contains. Writes past the single and
// double indirect blocks are refused (the disk is far too small to reach that
// point), and hard links are not created because there is no link() here.
//
// The inode a node names is kept alive while that node lives: an unlink drops
// the directory entry and the link count, but the inode's blocks are only
// released when the last in-memory node for it goes away. That is what makes
// "unlink a file that is still open" safe; the inode cannot be handed to a new
// file behind the open one's back.

namespace Ext2 {

constexpr uint16_t MAGIC = 0xEF53;
constexpr uint32_t INCOMPAT_FILETYPE = 0x0002;
constexpr uint32_t INCOMPAT_RECOVER  = 0x0004;
constexpr uint32_t INCOMPAT_EXTENTS  = 0x0040;
constexpr uint32_t INCOMPAT_64BIT = 0x0080;
constexpr uint32_t S_IFREG_BIT = 0100000;
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

// One device, however many mounts it has. Geometry and free counts live here
// rather than in a mount so that mounting the same disk twice (a read-only view
// and a writable layer, say) cannot end up with two answers to "how much is
// left". Nothing is ever unmounted, so the entry lives as long as the kernel.
struct Track;

struct Dev {
    Block::Device* dev;
    uint32_t block_size;
    uint32_t first_data_block;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t groups;
    uint32_t free_blocks;      // as in the superblock, kept current in memory
    uint32_t free_inodes;
    uint32_t feature_incompat;
    bool     has_filetype;
    Track*   tracked;          // live inode references held by Nodes
    Dev*     next;             // registry chain
};

// One mount: which device, which inode is its root, and whether it may write.
struct Fs {
    Dev* dev;
    VFS::Filesystem* vfs;      // the descriptor handed to the mount table
    uint32_t root_ino;
    bool writable;
};

// A node: the inode's identity and metadata, plus its block pointers, so that
// reading a file does not re-read its inode for every block. The write path
// refreshes this from the inode after every change it makes.
struct E2Node {
    uint32_t ino;
    bool     is_dir;
    uint64_t size;
    uint32_t mode;
    uint64_t mtime;
    uint32_t block[15];
};

// A live inode reference held by a Node, and whether the name that led to it
// has since been removed. The entry is created on the first Node for an inode
// and dropped when the last one is released; an orphan is freed at that point,
// never before.
struct Track {
    uint32_t ino;
    uint32_t refs;
    bool     orphan;
    bool     is_dir;
    Track*   next;
};

// A block on the stack; ext2 block sizes run 1K..4K and 4K is the ceiling we
// accept at mount time, so one block always fits.
constexpr uint32_t MAX_BLOCK = 4096;

// The timer can preempt a syscall halfway through a directory or bitmap update,
// so mutations run with interrupts off. Everything below that takes this lock is
// a top-level operation; the helpers assume it is already held.
static inline uint64_t irq_save() {
    uint64_t f;
    asm volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    if (f & (1ull << 9)) asm volatile("sti"); else asm volatile("cli");
}

// --- reading ---
// The transfer unit is the sector, so this is the one place partial-sector reads
// are assembled. Whole sectors go straight into the destination in a single
// request, which matters now that a real disk sits below this: a 1 KB block is
// one command, not two.
static bool read_bytes(Fs* f, uint64_t off, void* dst, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;

    // Up to the next sector boundary.
    while (n > 0 && (off % Block::SECTOR) != 0) {
        uint8_t sec[Block::SECTOR];
        if (f->dev->dev->read(f->dev->dev->priv, off / Block::SECTOR, 1, sec) != 1)
            return false;
        uint32_t boff = (uint32_t)(off % Block::SECTOR);
        uint32_t chunk = Block::SECTOR - boff;
        if (chunk > n) chunk = n;
        for (uint32_t i = 0; i < chunk; i++) d[i] = sec[boff + i];
        d += chunk;
        off += chunk;
        n -= chunk;
    }

    // Whole sectors, straight into the caller's buffer.
    uint32_t whole = n / Block::SECTOR;
    if (whole > 0) {
        if (f->dev->dev->read(f->dev->dev->priv, off / Block::SECTOR, whole, d) != whole)
            return false;
        d += (uint64_t)whole * Block::SECTOR;
        off += (uint64_t)whole * Block::SECTOR;
        n -= whole * Block::SECTOR;
    }

    // The remainder.
    if (n > 0) {
        uint8_t sec[Block::SECTOR];
        if (f->dev->dev->read(f->dev->dev->priv, off / Block::SECTOR, 1, sec) != 1)
            return false;
        for (uint32_t i = 0; i < n; i++) d[i] = sec[i];
    }
    return true;
}

// The mirror image, for metadata small enough to sit inside a sector. A partial
// first or last sector is read, patched and written back; whole sectors go out
// as one request.
static bool write_bytes(Fs* f, uint64_t off, const void* src, uint32_t n) {
    const uint8_t* s = (const uint8_t*)src;

    while (n > 0 && (off % Block::SECTOR) != 0) {
        uint8_t sec[Block::SECTOR];
        if (f->dev->dev->read(f->dev->dev->priv, off / Block::SECTOR, 1, sec) != 1)
            return false;
        uint32_t boff = (uint32_t)(off % Block::SECTOR);
        uint32_t chunk = Block::SECTOR - boff;
        if (chunk > n) chunk = n;
        for (uint32_t i = 0; i < chunk; i++) sec[boff + i] = s[i];
        if (f->dev->dev->write(f->dev->dev->priv, off / Block::SECTOR, 1, sec) != 1)
            return false;
        s += chunk;
        off += chunk;
        n -= chunk;
    }

    uint32_t whole = n / Block::SECTOR;
    if (whole > 0) {
        if (f->dev->dev->write(f->dev->dev->priv, off / Block::SECTOR, whole, s) != whole)
            return false;
        s += (uint64_t)whole * Block::SECTOR;
        off += (uint64_t)whole * Block::SECTOR;
        n -= whole * Block::SECTOR;
    }

    if (n > 0) {
        uint8_t sec[Block::SECTOR];
        if (f->dev->dev->read(f->dev->dev->priv, off / Block::SECTOR, 1, sec) != 1)
            return false;
        for (uint32_t i = 0; i < n; i++) sec[i] = s[i];
        if (f->dev->dev->write(f->dev->dev->priv, off / Block::SECTOR, 1, sec) != 1)
            return false;
    }
    return true;
}

static bool read_block(Fs* f, uint32_t blk, void* buf) {
    return read_bytes(f, (uint64_t)blk * f->dev->block_size, buf, f->dev->block_size);
}

static bool write_block(Fs* f, uint32_t blk, const void* buf) {
    return write_bytes(f, (uint64_t)blk * f->dev->block_size, buf, f->dev->block_size);
}

// A new block must not carry the bytes of whoever had it before. Written a
// sector at a time so this does not need a second block-sized buffer.
static bool zero_block(Fs* f, uint32_t blk) {
    uint8_t zero[Block::SECTOR];
    memset(zero, 0, sizeof(zero));
    uint64_t off = (uint64_t)blk * f->dev->block_size;
    for (uint32_t done = 0; done < f->dev->block_size; done += Block::SECTOR) {
        if (f->dev->dev->write(f->dev->dev->priv,
                               (off + done) / Block::SECTOR, 1, zero) != 1)
            return false;
    }
    return true;
}

// The group descriptor describes one group's bitmaps, inode table and counts.
// The whole filesystem has one group here, but the arithmetic is general.
static uint64_t gd_offset(Fs* f, uint32_t group) {
    return ((uint64_t)f->dev->first_data_block + 1) * f->dev->block_size
         + (uint64_t)group * sizeof(GroupDesc);
}

static bool read_gd(Fs* f, uint32_t group, GroupDesc* out) {
    if (group >= f->dev->groups) return false;
    return read_bytes(f, gd_offset(f, group), out, sizeof(GroupDesc));
}

static bool write_gd(Fs* f, uint32_t group, const GroupDesc* gd) {
    if (group >= f->dev->groups) return false;
    return write_bytes(f, gd_offset(f, group), gd, sizeof(GroupDesc));
}

// The superblock's free counts are the number every tool reads first, so they
// are written whenever a bitmap changes rather than left for a later sync.
static bool save_counts(Fs* f) {
    struct {
        uint32_t free_blocks;
        uint32_t free_inodes;
    } __attribute__((packed)) counts = {f->dev->free_blocks, f->dev->free_inodes};
    return write_bytes(f, 1024 + 12, &counts, sizeof(counts));
}

static bool read_inode(Fs* f, uint32_t ino, Inode* out) {
    if (ino == 0 || ino > f->dev->inodes_count) return false;
    uint32_t group = (ino - 1) / f->dev->inodes_per_group;
    uint32_t index = (ino - 1) % f->dev->inodes_per_group;
    if (group >= f->dev->groups) return false;

    GroupDesc gd;
    if (!read_gd(f, group, &gd)) return false;

    uint64_t off = (uint64_t)gd.inode_table * f->dev->block_size
                 + (uint64_t)index * f->dev->inode_size;
    // Only the standard prefix: the rest of a 256-byte inode belongs to the
    // fields this kernel does not model and must not be overwritten.
    return read_bytes(f, off, out, sizeof(Inode));
}

static bool write_inode(Fs* f, uint32_t ino, const Inode* in) {
    if (ino == 0 || ino > f->dev->inodes_count) return false;
    uint32_t group = (ino - 1) / f->dev->inodes_per_group;
    uint32_t index = (ino - 1) % f->dev->inodes_per_group;
    if (group >= f->dev->groups) return false;

    GroupDesc gd;
    if (!read_gd(f, group, &gd)) return false;

    uint64_t off = (uint64_t)gd.inode_table * f->dev->block_size
                 + (uint64_t)index * f->dev->inode_size;
    return write_bytes(f, off, in, sizeof(Inode));
}

static bool bitmap_bit(const uint8_t* map, uint32_t i) {
    return (map[i / 8] & (1u << (i % 8))) != 0;
}
static void bitmap_set(uint8_t* map, uint32_t i) {
    map[i / 8] |= (uint8_t)(1u << (i % 8));
}
static void bitmap_clear(uint8_t* map, uint32_t i) {
    map[i / 8] &= (uint8_t)~(1u << (i % 8));
}

// --- allocation ---

// Allocate one data or metadata block. Every allocation goes through the on-disk
// block bitmap, updates both the group descriptor and the superblock count, and
// clears the block before returning it: a reader must never see stale bytes.
static uint32_t alloc_block(Fs* f) {
    Dev* d = f->dev;
    uint32_t buf[MAX_BLOCK / 4];

    for (uint32_t g = 0; g < d->groups; g++) {
        GroupDesc gd;
        if (!read_gd(f, g, &gd)) return 0;
        if (gd.free_blocks_count == 0) continue;
        if (!read_block(f, gd.block_bitmap, buf)) return 0;

        uint8_t* map = (uint8_t*)buf;
        uint32_t base = d->first_data_block + g * d->blocks_per_group;
        uint32_t limit = d->block_size * 8;
        if (limit > d->blocks_per_group) limit = d->blocks_per_group;
        for (uint32_t i = 0; i < limit; i++) {
            uint32_t blk = base + i;
            if (blk >= d->blocks_count) break;
            if (bitmap_bit(map, i)) continue;

            bitmap_set(map, i);
            if (!write_block(f, gd.block_bitmap, buf)) return 0;
            gd.free_blocks_count--;
            if (!write_gd(f, g, &gd)) return 0;
            d->free_blocks--;
            save_counts(f);
            if (!zero_block(f, blk)) return 0;
            return blk;
        }
    }
    return 0;   // out of room
}

static void free_block(Fs* f, uint32_t blk) {
    Dev* d = f->dev;
    if (blk < d->first_data_block || blk >= d->blocks_count) return;

    uint32_t rel = blk - d->first_data_block;
    uint32_t g = rel / d->blocks_per_group;
    uint32_t i = rel % d->blocks_per_group;

    GroupDesc gd;
    if (!read_gd(f, g, &gd)) return;

    uint32_t buf[MAX_BLOCK / 4];
    if (!read_block(f, gd.block_bitmap, buf)) return;

    uint8_t* map = (uint8_t*)buf;
    if (!bitmap_bit(map, i)) return;   // already free: never double-count it
    bitmap_clear(map, i);
    if (!write_block(f, gd.block_bitmap, buf)) return;
    gd.free_blocks_count++;
    write_gd(f, g, &gd);
    d->free_blocks++;
    save_counts(f);
}

static uint32_t alloc_inode(Fs* f, bool is_dir) {
    Dev* d = f->dev;
    uint32_t buf[MAX_BLOCK / 4];

    for (uint32_t g = 0; g < d->groups; g++) {
        GroupDesc gd;
        if (!read_gd(f, g, &gd)) return 0;
        if (gd.free_inodes_count == 0) continue;
        if (!read_block(f, gd.inode_bitmap, buf)) return 0;

        uint8_t* map = (uint8_t*)buf;
        uint32_t limit = d->block_size * 8;
        if (limit > d->inodes_per_group) limit = d->inodes_per_group;
        for (uint32_t i = 0; i < limit; i++) {
            uint32_t ino = g * d->inodes_per_group + i + 1;
            if (ino > d->inodes_count) break;
            if (bitmap_bit(map, i)) continue;

            bitmap_set(map, i);
            if (!write_block(f, gd.inode_bitmap, buf)) return 0;
            gd.free_inodes_count--;
            if (is_dir) gd.used_dirs_count++;
            if (!write_gd(f, g, &gd)) return 0;
            d->free_inodes--;
            save_counts(f);

            // A fresh inode: zero the standard prefix. The extra bytes beyond it
            // are left as the image made them.
            Inode zin;
            memset(&zin, 0, sizeof(zin));
            if (!write_inode(f, ino, &zin)) return 0;
            return ino;
        }
    }
    return 0;
}

// Free every block an inode owns, including the indirect pointer blocks. The
// pointers are cleared in memory but the inode itself is written by the caller.
static void free_indirect(Fs* f, uint32_t blk, int level) {
    if (blk == 0) return;
    uint32_t buf[MAX_BLOCK / 4];
    if (!read_block(f, blk, buf)) return;
    const uint32_t ptrs = f->dev->block_size / 4;
    for (uint32_t i = 0; i < ptrs; i++) {
        if (buf[i] == 0) continue;
        if (level > 1) free_indirect(f, buf[i], level - 1);
        else free_block(f, buf[i]);
    }
    free_block(f, blk);
}

static void free_inode_blocks(Fs* f, Inode* in) {
    for (int i = 0; i < 12; i++) {
        if (in->block[i]) { free_block(f, in->block[i]); in->block[i] = 0; }
    }
    free_indirect(f, in->block[12], 1);
    free_indirect(f, in->block[13], 2);
    free_indirect(f, in->block[14], 3);
    in->block[12] = in->block[13] = in->block[14] = 0;
}

// Clear the bitmap bit and the counts, then leave a deleted inode behind: no
// links, no blocks, and a dtime, which is what e2fsck expects of a free inode.
static void free_inode(Fs* f, uint32_t ino, bool is_dir) {
    Dev* d = f->dev;
    if (ino == 0 || ino > d->inodes_count) return;

    uint32_t g = (ino - 1) / d->inodes_per_group;
    uint32_t i = (ino - 1) % d->inodes_per_group;

    // Free the blocks first. Each free updates the group descriptor, so reading
    // it before this loop and writing it afterwards would put the old block
    // count back and lose every increment the loop made.
    Inode in;
    if (read_inode(f, ino, &in)) {
        free_inode_blocks(f, &in);
        in.size = 0;
        in.blocks = 0;
        in.links_count = 0;
        in.dtime = (uint32_t)Rtc::seconds_now();
        if (in.dtime == 0) in.dtime = 1;
        write_inode(f, ino, &in);
    }

    GroupDesc gd;
    if (!read_gd(f, g, &gd)) return;

    uint32_t buf[MAX_BLOCK / 4];
    if (!read_block(f, gd.inode_bitmap, buf)) return;
    uint8_t* map = (uint8_t*)buf;
    if (!bitmap_bit(map, i)) return;   // already free
    bitmap_clear(map, i);
    if (!write_block(f, gd.inode_bitmap, buf)) return;
    gd.free_inodes_count++;
    if (is_dir && gd.used_dirs_count > 0) gd.used_dirs_count--;
    write_gd(f, g, &gd);
    d->free_inodes++;
    save_counts(f);
}

// --- block pointers: direct, single and double indirect ---

// The read path's mapper: the physical block holding file block 'idx', or 0 for
// a hole.
static uint32_t map_block(Fs* f, const uint32_t* bp, uint32_t idx) {
    uint32_t buf[MAX_BLOCK / 4];
    const uint32_t ptrs = f->dev->block_size / 4;

    if (idx < 12) return bp[idx];
    idx -= 12;

    if (idx < ptrs) {
        if (bp[12] == 0) return 0;
        if (!read_bytes(f, (uint64_t)bp[12] * f->dev->block_size, buf, f->dev->block_size))
            return 0;
        return buf[idx];
    }
    idx -= ptrs;

    if (idx < ptrs * ptrs) {
        if (bp[13] == 0) return 0;
        if (!read_bytes(f, (uint64_t)bp[13] * f->dev->block_size, buf, f->dev->block_size))
            return 0;
        uint32_t mid = buf[idx / ptrs];
        if (mid == 0) return 0;
        if (!read_bytes(f, (uint64_t)mid * f->dev->block_size, buf, f->dev->block_size))
            return 0;
        return buf[idx % ptrs];
    }
    return 0;   // triple indirect: beyond anything this mount is asked for
}

// The write path's mapper. With 'create' it allocates the data block and any
// pointer block on the way to it; pointer blocks are allocated lazily, so a
// sparse file costs nothing until something is written into its range. Sets
// *err when the index is past double indirect (refused: ENOSPC).
static uint32_t get_block(Fs* f, Inode* in, uint32_t idx, bool create, bool* err) {
    const uint32_t ptrs = f->dev->block_size / 4;
    uint32_t buf[MAX_BLOCK / 4];

    if (idx < 12) {
        if (in->block[idx] == 0 && create) {
            uint32_t b = alloc_block(f);
            if (b == 0) { *err = true; return 0; }
            in->block[idx] = b;
        }
        return in->block[idx];
    }
    idx -= 12;

    if (idx < ptrs) {
        if (in->block[12] == 0) {
            if (!create) return 0;
            uint32_t b = alloc_block(f);
            if (b == 0) { *err = true; return 0; }
            in->block[12] = b;
        }
        if (!read_block(f, in->block[12], buf)) return 0;
        if (buf[idx] == 0 && create) {
            uint32_t b = alloc_block(f);
            if (b == 0) { *err = true; return 0; }
            buf[idx] = b;
            write_block(f, in->block[12], buf);
        }
        return buf[idx];
    }
    idx -= ptrs;

    if (idx < ptrs * ptrs) {
        if (in->block[13] == 0) {
            if (!create) return 0;
            uint32_t b = alloc_block(f);
            if (b == 0) { *err = true; return 0; }
            in->block[13] = b;
        }
        if (!read_block(f, in->block[13], buf)) return 0;
        uint32_t mid = buf[idx / ptrs];
        if (mid == 0) {
            if (!create) return 0;
            mid = alloc_block(f);
            if (mid == 0) { *err = true; return 0; }
            buf[idx / ptrs] = mid;
            write_block(f, in->block[13], buf);
        }
        if (!read_block(f, mid, buf)) return 0;
        if (buf[idx % ptrs] == 0 && create) {
            uint32_t b = alloc_block(f);
            if (b == 0) { *err = true; return 0; }
            buf[idx % ptrs] = b;
            write_block(f, mid, buf);
        }
        return buf[idx % ptrs];
    }

    *err = true;   // triple indirect: this kernel will not build one
    return 0;
}

// Drop one file block's pointer, returning the block it named (0 for a hole).
// The pointer block itself stays; pruning it is separate business.
static uint32_t unmap_block(Fs* f, Inode* in, uint32_t idx) {
    const uint32_t ptrs = f->dev->block_size / 4;
    uint32_t buf[MAX_BLOCK / 4];

    if (idx < 12) {
        uint32_t b = in->block[idx];
        in->block[idx] = 0;
        return b;
    }
    idx -= 12;

    if (idx < ptrs) {
        if (in->block[12] == 0) return 0;
        if (!read_block(f, in->block[12], buf)) return 0;
        uint32_t b = buf[idx];
        if (b != 0) { buf[idx] = 0; write_block(f, in->block[12], buf); }
        return b;
    }
    idx -= ptrs;

    if (idx < ptrs * ptrs) {
        if (in->block[13] == 0) return 0;
        if (!read_block(f, in->block[13], buf)) return 0;
        uint32_t mid = buf[idx / ptrs];
        if (mid == 0) return 0;
        uint32_t inner[MAX_BLOCK / 4];
        if (!read_block(f, mid, inner)) return 0;
        uint32_t b = inner[idx % ptrs];
        if (b != 0) { inner[idx % ptrs] = 0; write_block(f, mid, inner); }
        return b;
    }
    return 0;
}

// Release pointer blocks that no longer have a live entry below them, after the
// data blocks past the new end have been unmapped. 'kept' is the file's new
// block count.
static void prune_indirect(Fs* f, Inode* in, uint32_t kept) {
    const uint32_t ptrs = f->dev->block_size / 4;
    uint32_t buf[MAX_BLOCK / 4];

    // Single indirect covers file blocks [12, 12 + ptrs).
    if (in->block[12] != 0 && kept <= 12) {
        if (read_block(f, in->block[12], buf))
            for (uint32_t i = 0; i < ptrs; i++)
                if (buf[i]) { free_block(f, buf[i]); buf[i] = 0; }
        free_block(f, in->block[12]);
        in->block[12] = 0;
    }

    // Double indirect covers [12 + ptrs, 12 + ptrs + ptrs*ptrs).
    if (in->block[13] == 0) return;
    const uint32_t base = 12 + ptrs;
    if (kept <= base) {
        if (read_block(f, in->block[13], buf)) {
            for (uint32_t i = 0; i < ptrs; i++) {
                uint32_t mid = buf[i];
                if (mid == 0) continue;
                uint32_t inner[MAX_BLOCK / 4];
                if (read_block(f, mid, inner))
                    for (uint32_t k = 0; k < ptrs; k++)
                        if (inner[k]) { free_block(f, inner[k]); inner[k] = 0; }
                free_block(f, mid);
                buf[i] = 0;
            }
        }
        free_block(f, in->block[13]);
        in->block[13] = 0;
        return;
    }
    if (!read_block(f, in->block[13], buf)) return;
    bool dirty = false;
    for (uint32_t i = 0; i < ptrs; i++) {
        uint32_t mid = buf[i];
        if (mid == 0) continue;
        if (base + i * ptrs >= kept) {
            uint32_t inner[MAX_BLOCK / 4];
            if (read_block(f, mid, inner))
                for (uint32_t k = 0; k < ptrs; k++)
                    if (inner[k]) { free_block(f, inner[k]); inner[k] = 0; }
            free_block(f, mid);
            buf[i] = 0;
            dirty = true;
        }
    }
    if (dirty) write_block(f, in->block[13], buf);
}

// How many filesystem blocks the inode owns, pointer blocks included. i_blocks
// is reported in 512-byte units, the way ext2 stores it.
static uint32_t count_blocks(Fs* f, const Inode* in) {
    const uint32_t ptrs = f->dev->block_size / 4;
    uint32_t n = 0;
    uint32_t buf[MAX_BLOCK / 4];

    for (int i = 0; i < 12; i++)
        if (in->block[i]) n++;

    if (in->block[12] && read_block(f, in->block[12], buf)) {
        n++;
        for (uint32_t i = 0; i < ptrs; i++) if (buf[i]) n++;
    }

    if (in->block[13] && read_block(f, in->block[13], buf)) {
        n++;
        for (uint32_t i = 0; i < ptrs; i++) {
            uint32_t mid = buf[i];
            if (mid == 0) continue;
            n++;
            uint32_t inner[MAX_BLOCK / 4];
            if (read_block(f, mid, inner))
                for (uint32_t k = 0; k < ptrs; k++) if (inner[k]) n++;
        }
    }

    if (in->block[14]) n++;   // not built here, but it is a block if present
    return n;
}

static uint32_t blocks_to_512(Fs* f, uint32_t blocks) {
    return blocks * (f->dev->block_size / 512);
}

// --- the in-memory inode reference table ---

static Track* track_find(Dev* d, uint32_t ino) {
    for (Track* t = d->tracked; t; t = t->next)
        if (t->ino == ino) return t;
    return nullptr;
}

static bool track_acquire(Fs* f, uint32_t ino, bool is_dir) {
    uint64_t flags = irq_save();
    Track* t = track_find(f->dev, ino);
    if (t == nullptr) {
        t = new Track;
        if (t == nullptr) { irq_restore(flags); return false; }
        t->ino = ino;
        t->refs = 0;
        t->orphan = false;
        t->is_dir = is_dir;
        t->next = f->dev->tracked;
        f->dev->tracked = t;
    }
    t->refs++;
    irq_restore(flags);
    return true;
}

// The last node for an inode has gone. If the name went first, this is where
// the blocks and the inode are actually released; until now they had to stay.
static void track_release(Fs* f, uint32_t ino) {
    uint64_t flags = irq_save();
    Track** pp = &f->dev->tracked;
    while (*pp) {
        Track* t = *pp;
        if (t->ino != ino) { pp = &t->next; continue; }
        if (t->refs > 1) { t->refs--; irq_restore(flags); return; }
        if (t->orphan) free_inode(f, ino, t->is_dir);
        *pp = t->next;
        delete t;
        irq_restore(flags);
        return;
    }
    irq_restore(flags);
}

static int track_refs(Dev* d, uint32_t ino) {
    Track* t = track_find(d, ino);
    return t ? (int)t->refs : 0;
}

static void track_orphan(Fs* f, uint32_t ino) {
    Track* t = track_find(f->dev, ino);
    if (t) t->orphan = true;
}

// --- nodes ---

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

    if (!track_acquire(f, ino, n->is_dir)) { delete n; return nullptr; }

    VFS::Node* v = VFS::node_new(f->vfs, n->is_dir ? VFS::FT_DIR : VFS::FT_FILE);
    if (v == nullptr) {
        track_release(f, ino);
        delete n;
        return nullptr;
    }
    v->priv = n;
    v->size = n->size;
    v->ino = n->ino;
    v->mode = n->mode;
    v->mtime = n->mtime;
    return v;
}

// Copy an inode's mutable state back onto a node, so reads through the same
// descriptor and fstat see what the last write did.
static void refresh_node(E2Node* n, VFS::Node* v, const Inode* in) {
    n->size = in->size;
    n->mtime = in->mtime;
    for (int k = 0; k < 15; k++) n->block[k] = in->block[k];
    if (v) {
        v->size = in->size;
        v->mtime = in->mtime;
    }
}

// --- directory records ---
// A record is a header, a name, and enough padding that the next record starts
// on a four-byte boundary. rec_len is the distance to the next record, so a
// record with slack can be split, and two adjacent free records can be merged.

static uint32_t rec_used(uint8_t name_len) {
    return (8 + (uint32_t)name_len + 3) & ~3u;
}

// The smallest record e2fsck will accept, empty or not. Splitting a freed
// record down to the bare 8-byte header is legal on paper but e2fsck calls the
// directory corrupt, so a split never leaves an empty record shorter than this.
static const uint32_t MIN_DIR_REC = 12;

static bool name_valid(const char* name, uint32_t* out_len) {
    uint32_t len = 0;
    while (name[len]) {
        if (name[len] == '/') return false;
        len++;
        if (len > 255) return false;
    }
    if (len == 0) return false;
    if (len == 1 && name[0] == '.') return false;
    if (len == 2 && name[0] == '.' && name[1] == '.') return false;
    *out_len = len;
    return true;
}

static bool name_eq(const char* a, const char* b, uint32_t len) {
    for (uint32_t i = 0; i < len; i++)
        if (a[i] != b[i]) return false;
    return true;
}

// Walk a directory's data blocks, handing each record to 'visit'. The walker is
// the common part of lookup and readdir; both need the same linear pass.
template <typename F>
static bool walk_dir(Fs* f, E2Node* d, F visit) {
    uint32_t buf[MAX_BLOCK / 4];
    uint32_t nblocks = (uint32_t)((d->size + f->dev->block_size - 1) / f->dev->block_size);

    for (uint32_t b = 0; b < nblocks; b++) {
        uint32_t blk = map_block(f, d->block, b);
        if (blk == 0) continue;
        if (!read_bytes(f, (uint64_t)blk * f->dev->block_size, buf, f->dev->block_size))
            return false;

        uint8_t* raw = (uint8_t*)buf;
        uint32_t p = 0;
        while (p + sizeof(RawDirEntry) <= f->dev->block_size) {
            RawDirEntry* de = (RawDirEntry*)(raw + p);
            if (de->rec_len < sizeof(RawDirEntry) ||
                p + de->rec_len > f->dev->block_size)
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

// Locate one record. On success 'out_block' is the (physical) block it lives
// in and 'out_off' the byte offset within that block.
static bool dir_find(Fs* f, Inode* dir, const char* name, uint32_t len,
                     uint32_t* out_ino, uint32_t* out_block, uint32_t* out_off,
                     uint8_t* out_type) {
    uint32_t bs = f->dev->block_size;
    uint32_t buf[MAX_BLOCK / 4];
    uint32_t nblocks = (dir->size + bs - 1) / bs;

    for (uint32_t b = 0; b < nblocks; b++) {
        bool err = false;
        uint32_t blk = get_block(f, dir, b, false, &err);
        if (blk == 0) continue;
        if (!read_block(f, blk, buf)) return false;
        uint8_t* raw = (uint8_t*)buf;
        uint32_t p = 0;
        while (p + sizeof(RawDirEntry) <= bs) {
            RawDirEntry* de = (RawDirEntry*)(raw + p);
            if (de->rec_len < sizeof(RawDirEntry) || p + de->rec_len > bs) break;
            if (de->inode != 0 && de->name_len == len &&
                name_eq((const char*)(raw + p + sizeof(RawDirEntry)), name, len)) {
                if (out_ino) *out_ino = de->inode;
                if (out_block) *out_block = blk;
                if (out_off) *out_off = p;
                if (out_type) *out_type = de->file_type;
                return true;
            }
            p += de->rec_len;
        }
    }
    return false;
}

// Add a record, splitting the first record with room or appending a block. The
// directory's inode is updated in memory; the caller writes it.
static bool dir_insert(Fs* f, Inode* dir, const char* name, uint32_t len,
                       uint32_t ino, uint8_t ftype) {
    uint32_t bs = f->dev->block_size;
    uint32_t needed = rec_used((uint8_t)len);
    uint32_t buf[MAX_BLOCK / 4];
    uint32_t nblocks = (dir->size + bs - 1) / bs;

    for (uint32_t b = 0; b < nblocks; b++) {
        bool err = false;
        uint32_t blk = get_block(f, dir, b, false, &err);
        if (blk == 0) continue;
        if (!read_block(f, blk, buf)) return false;
        uint8_t* raw = (uint8_t*)buf;
        uint32_t p = 0;
        while (p + sizeof(RawDirEntry) <= bs) {
            RawDirEntry* de = (RawDirEntry*)(raw + p);
            if (de->rec_len < sizeof(RawDirEntry) || p + de->rec_len > bs) break;

            uint32_t used = de->inode == 0 ? MIN_DIR_REC
                                           : rec_used(de->name_len);
            if (de->rec_len >= used + needed) {
                uint16_t old_rec = de->rec_len;
                de->rec_len = (uint16_t)used;

                RawDirEntry* nd = (RawDirEntry*)(raw + p + used);
                nd->inode = ino;
                nd->rec_len = (uint16_t)(old_rec - used);
                nd->name_len = (uint8_t)len;
                nd->file_type = ftype;
                memset((uint8_t*)nd + sizeof(RawDirEntry), 0,
                       needed - sizeof(RawDirEntry));
                memcpy((uint8_t*)nd + sizeof(RawDirEntry), name, len);
                return write_block(f, blk, buf);
            }
            p += de->rec_len;
        }
    }

    // No record has room: give the directory another block. Its records fill
    // the block, so a fresh block holds exactly one record covering it all.
    bool grew = false;
    uint32_t blk = get_block(f, dir, nblocks, true, &grew);
    if (blk == 0) return false;
    memset(buf, 0, bs);
    RawDirEntry* de = (RawDirEntry*)buf;
    de->inode = ino;
    de->rec_len = (uint16_t)bs;
    de->name_len = (uint8_t)len;
    de->file_type = ftype;
    memcpy((uint8_t*)buf + sizeof(RawDirEntry), name, len);
    if (!write_block(f, blk, buf)) return false;

    dir->size += bs;
    return true;
}

// Remove one record, merging it with a free neighbour so the block does not
// fragment into records too small to use.
static bool dir_remove(Fs* f, Inode* dir, const char* name, uint32_t len,
                       uint32_t* out_ino, uint8_t* out_type) {
    uint32_t bs = f->dev->block_size;
    uint32_t buf[MAX_BLOCK / 4];
    uint32_t nblocks = (dir->size + bs - 1) / bs;

    for (uint32_t b = 0; b < nblocks; b++) {
        bool err = false;
        uint32_t blk = get_block(f, dir, b, false, &err);
        if (blk == 0) continue;
        if (!read_block(f, blk, buf)) return false;

        uint8_t* raw = (uint8_t*)buf;
        uint32_t p = 0, prev = 0;
        bool have_prev = false;
        while (p + sizeof(RawDirEntry) <= bs) {
            RawDirEntry* de = (RawDirEntry*)(raw + p);
            if (de->rec_len < sizeof(RawDirEntry) || p + de->rec_len > bs) break;
            if (de->inode != 0 && de->name_len == len &&
                name_eq((const char*)(raw + p + sizeof(RawDirEntry)), name, len)) {
                if (out_ino) *out_ino = de->inode;
                if (out_type) *out_type = de->file_type;
                de->inode = 0;
                de->name_len = 0;
                de->file_type = 0;

                // Merge with the record after it when that one is free.
                uint32_t next = p + de->rec_len;
                if (next + sizeof(RawDirEntry) <= bs) {
                    RawDirEntry* nx = (RawDirEntry*)(raw + next);
                    if (nx->inode == 0 && nx->rec_len >= sizeof(RawDirEntry) &&
                        next + nx->rec_len <= bs) {
                        de->rec_len = (uint16_t)(de->rec_len + nx->rec_len);
                    }
                }
                // And with the record before it, which may now be the last.
                if (have_prev) {
                    RawDirEntry* pv = (RawDirEntry*)(raw + prev);
                    if (pv->inode == 0) {
                        pv->rec_len = (uint16_t)(pv->rec_len + de->rec_len);
                    }
                }
                return write_block(f, blk, buf);
            }
            prev = p;
            have_prev = true;
            p += de->rec_len;
        }
    }
    return false;
}

static bool dir_is_empty(Fs* f, Inode* dir) {
    uint32_t bs = f->dev->block_size;
    uint32_t buf[MAX_BLOCK / 4];
    uint32_t nblocks = (dir->size + bs - 1) / bs;

    for (uint32_t b = 0; b < nblocks; b++) {
        bool err = false;
        uint32_t blk = get_block(f, dir, b, false, &err);
        if (blk == 0) continue;
        if (!read_block(f, blk, buf)) return false;
        uint8_t* raw = (uint8_t*)buf;
        uint32_t p = 0;
        while (p + sizeof(RawDirEntry) <= bs) {
            RawDirEntry* de = (RawDirEntry*)(raw + p);
            if (de->rec_len < sizeof(RawDirEntry) || p + de->rec_len > bs) break;
            if (de->inode != 0) {
                const char* n = (const char*)(raw + p + sizeof(RawDirEntry));
                bool dot = (de->name_len == 1 && n[0] == '.') ||
                           (de->name_len == 2 && n[0] == '.' && n[1] == '.');
                if (!dot) return false;
            }
            p += de->rec_len;
        }
    }
    return true;
}

// The inode a directory's ".." names. This is the rename cycle check's edge.
static bool dotdot_ino(Fs* f, Inode* dir, uint32_t* out) {
    bool err = false;
    uint32_t blk = get_block(f, dir, 0, false, &err);
    if (blk == 0) return false;
    uint32_t buf[MAX_BLOCK / 4];
    if (!read_block(f, blk, buf)) return false;
    uint8_t* raw = (uint8_t*)buf;
    uint32_t p = 0;
    while (p + sizeof(RawDirEntry) <= f->dev->block_size) {
        RawDirEntry* de = (RawDirEntry*)(raw + p);
        if (de->rec_len < sizeof(RawDirEntry) || p + de->rec_len > f->dev->block_size)
            break;
        if (de->name_len == 2) {
            const char* n = (const char*)(raw + p + sizeof(RawDirEntry));
            if (n[0] == '.' && n[1] == '.') { *out = de->inode; return true; }
        }
        p += de->rec_len;
    }
    return false;
}

// Point a directory's ".." at its new parent. Only the parent's inode number
// changes; the record keeps its length.
static bool set_dotdot(Fs* f, Inode* dir, uint32_t parent) {
    bool err = false;
    uint32_t blk = get_block(f, dir, 0, false, &err);
    if (blk == 0) return false;
    uint32_t buf[MAX_BLOCK / 4];
    if (!read_block(f, blk, buf)) return false;
    uint8_t* raw = (uint8_t*)buf;
    uint32_t p = 0;
    while (p + sizeof(RawDirEntry) <= f->dev->block_size) {
        RawDirEntry* de = (RawDirEntry*)(raw + p);
        if (de->rec_len < sizeof(RawDirEntry) || p + de->rec_len > f->dev->block_size)
            break;
        if (de->name_len == 2) {
            const char* n = (const char*)(raw + p + sizeof(RawDirEntry));
            if (n[0] == '.' && n[1] == '.') {
                de->inode = parent;
                return write_block(f, blk, buf);
            }
        }
        p += de->rec_len;
    }
    return false;
}

// A new directory starts with its two mandatory records.
static bool write_dot_entries(Fs* f, uint32_t blk, uint32_t self, uint32_t parent) {
    uint32_t bs = f->dev->block_size;
    uint32_t buf[MAX_BLOCK / 4];
    memset(buf, 0, bs);
    uint8_t* raw = (uint8_t*)buf;

    RawDirEntry* dot = (RawDirEntry*)raw;
    dot->inode = self;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = f->dev->has_filetype ? 2 : 0;   // 2 = directory
    raw[sizeof(RawDirEntry)] = '.';

    RawDirEntry* dotdot = (RawDirEntry*)(raw + 12);
    dotdot->inode = parent;
    dotdot->rec_len = (uint16_t)(bs - 12);
    dotdot->name_len = 2;
    dotdot->file_type = f->dev->has_filetype ? 2 : 0;
    raw[12 + sizeof(RawDirEntry)] = '.';
    raw[12 + sizeof(RawDirEntry) + 1] = '.';

    return write_block(f, blk, buf);
}

// --- the ops ---

static VFS::Node* e2_root(void* priv) {
    Fs* f = (Fs*)priv;
    return make_node(f, f->root_ino);
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
    uint32_t bs = f->dev->block_size;
    uint64_t done = 0;
    while (done < count) {
        uint64_t pos = offset + done;
        uint32_t idx = (uint32_t)(pos / bs);
        uint32_t boff = (uint32_t)(pos % bs);
        uint32_t chunk = bs - boff;
        if (chunk > count - done) chunk = (uint32_t)(count - done);

        uint32_t blk = map_block(f, n->block, idx);
        if (blk == 0) {
            // A hole reads as zeroes. Stopping here would turn a sparse file
            // into a short read.
            memset(dst + done, 0, chunk);
            done += chunk;
            continue;
        }
        if (!read_bytes(f, (uint64_t)blk * bs + boff, dst + done, chunk))
            break;
        done += chunk;
    }
    return done;
}

static uint64_t e2_write(void* priv, VFS::Node* node, const void* buf,
                         uint64_t count, uint64_t offset) {
    Fs* f = (Fs*)priv;
    E2Node* n = (E2Node*)node->priv;
    if (!f->writable || n->is_dir || count == 0) return 0;

    uint64_t flags = irq_save();

    Inode in;
    if (!read_inode(f, n->ino, &in)) { irq_restore(flags); return 0; }

    const uint8_t* src = (const uint8_t*)buf;
    uint32_t bs = f->dev->block_size;
    uint64_t done = 0;
    bool changed = false;

    while (done < count) {
        uint64_t pos = offset + done;
        uint32_t idx = (uint32_t)(pos / bs);
        uint32_t boff = (uint32_t)(pos % bs);
        uint32_t chunk = bs - boff;
        if (chunk > count - done) chunk = (uint32_t)(count - done);

        bool err = false;
        uint32_t blk = get_block(f, &in, idx, true, &err);
        if (blk == 0) break;   // out of room (or triple indirect): stop
        changed = true;

        if (boff == 0 && chunk == bs) {
            if (!write_block(f, blk, src + done)) break;
        } else {
            uint32_t tmp[MAX_BLOCK / 4];
            if (!read_block(f, blk, tmp)) break;
            uint8_t* t = (uint8_t*)tmp;
            // Writing past the old end inside an existing block: everything
            // between the old size and this offset is a hole and must read back
            // as zero, even if the block came back from an earlier shrink.
            if (pos > in.size && idx == (uint32_t)(in.size / bs)) {
                uint32_t gap_start = (uint32_t)(in.size % bs);
                if (gap_start < boff)
                    memset(t + gap_start, 0, boff - gap_start);
            }
            memcpy(t + boff, src + done, chunk);
            if (!write_block(f, blk, tmp)) break;
        }
        done += chunk;
    }

    if (changed) {
        if (offset + done > in.size) in.size = (uint32_t)(offset + done);
        uint32_t now = (uint32_t)Rtc::seconds_now();
        in.mtime = now;
        in.ctime = now;
        in.blocks = blocks_to_512(f, count_blocks(f, &in));
        write_inode(f, n->ino, &in);
        refresh_node(n, node, &in);
    }
    irq_restore(flags);
    return done;   // 0 means nothing could be allocated: ENOSPC above
}

static int e2_truncate(void* priv, VFS::Node* node, uint64_t size) {
    Fs* f = (Fs*)priv;
    E2Node* n = (E2Node*)node->priv;
    if (!f->writable || n->is_dir) return -1;
    if (size > 0xFFFFFFFFull) return -1;   // i_size is 32 bits here

    uint64_t flags = irq_save();

    Inode in;
    if (!read_inode(f, n->ino, &in)) { irq_restore(flags); return -1; }
    uint32_t bs = f->dev->block_size;

    if (size < in.size) {
        uint32_t kept = (uint32_t)((size + bs - 1) / bs);

        // Zero the tail of the last kept block before giving the rest away, so
        // a later grow through that block sees zeroes and not old bytes.
        uint32_t tail = (uint32_t)(size % bs);
        if (tail != 0) {
            uint32_t blk = map_block(f, in.block, (uint32_t)(size / bs));
            if (blk != 0) {
                uint32_t buf[MAX_BLOCK / 4];
                if (read_block(f, blk, buf)) {
                    memset((uint8_t*)buf + tail, 0, bs - tail);
                    write_block(f, blk, buf);
                }
            }
        }

        uint32_t old = (in.size + bs - 1) / bs;
        for (uint32_t idx = kept; idx < old; idx++) {
            uint32_t b = unmap_block(f, &in, idx);
            if (b) free_block(f, b);
        }
        prune_indirect(f, &in, kept);
        in.size = (uint32_t)size;
    } else {
        // Growing is sparse: the tail reads as zeroes because a hole does.
        in.size = (uint32_t)size;
    }

    uint32_t now = (uint32_t)Rtc::seconds_now();
    in.mtime = now;
    in.ctime = now;
    in.blocks = blocks_to_512(f, count_blocks(f, &in));
    write_inode(f, n->ino, &in);
    refresh_node(n, node, &in);

    irq_restore(flags);
    return 0;
}

static int e2_create(void* priv, VFS::Node* dir, const char* name, VFS::FileType type,
                     VFS::Node** out) {
    Fs* f = (Fs*)priv;
    if (!f->writable || dir->type != VFS::FT_DIR) return -1;
    uint32_t len = 0;
    if (!name_valid(name, &len)) return -1;

    uint64_t flags = irq_save();

    E2Node* d = (E2Node*)dir->priv;
    Inode din;
    if (!read_inode(f, d->ino, &din)) { irq_restore(flags); return -1; }
    if ((din.mode & S_IFDIR_BIT) == 0) { irq_restore(flags); return -1; }

    // The VFS looks first, but the backend owns the namespace: a create must
    // not overwrite an entry that appeared in the meantime.
    if (dir_find(f, &din, name, len, nullptr, nullptr, nullptr, nullptr)) {
        irq_restore(flags);
        return -1;
    }

    bool is_dir = type == VFS::FT_DIR;
    uint32_t ino = alloc_inode(f, is_dir);
    if (ino == 0) { irq_restore(flags); return -1; }

    Inode in;
    memset(&in, 0, sizeof(in));
    in.mode = (uint16_t)(is_dir ? (S_IFDIR_BIT | 0755) : (S_IFREG_BIT | 0644));
    in.links_count = is_dir ? 2 : 1;
    uint32_t now = (uint32_t)Rtc::seconds_now();
    in.atime = in.ctime = in.mtime = now;

    uint32_t dir_blk = 0;
    if (is_dir) {
        dir_blk = alloc_block(f);
        if (dir_blk == 0) {
            free_inode(f, ino, true);
            irq_restore(flags);
            return -1;
        }
        in.block[0] = dir_blk;
        in.size = f->dev->block_size;
        in.blocks = blocks_to_512(f, 1);
        if (!write_dot_entries(f, dir_blk, ino, d->ino)) {
            free_block(f, dir_blk);
            free_inode(f, ino, true);
            irq_restore(flags);
            return -1;
        }
    }
    if (!write_inode(f, ino, &in)) {
        if (dir_blk) free_block(f, dir_blk);
        free_inode(f, ino, is_dir);
        irq_restore(flags);
        return -1;
    }

    uint8_t ftype = f->dev->has_filetype ? (is_dir ? 2 : 1) : 0;
    if (!dir_insert(f, &din, name, len, ino, ftype)) {
        // dir_insert may have added a block to the parent first; keep the
        // parent's view consistent even on failure.
        write_inode(f, d->ino, &din);
        refresh_node(d, dir, &din);
        free_inode(f, ino, is_dir);
        irq_restore(flags);
        return -1;
    }

    if (is_dir) din.links_count++;   // the new directory's ".." names us
    din.mtime = now;
    din.ctime = now;
    din.blocks = blocks_to_512(f, count_blocks(f, &din));
    write_inode(f, d->ino, &din);
    refresh_node(d, dir, &din);

    VFS::Node* made = make_node(f, ino);
    irq_restore(flags);
    if (made == nullptr) return -1;
    *out = made;
    return 0;
}

// Drop the inode itself, or defer it if a live node still names it. A deferred
// inode keeps its blocks: the open file may still be written or read.
static void inode_put(Fs* f, uint32_t ino, bool is_dir) {
    if (track_refs(f->dev, ino) > 0) track_orphan(f, ino);
    else free_inode(f, ino, is_dir);
}

static int e2_unlink(void* priv, VFS::Node* dir, const char* name) {
    Fs* f = (Fs*)priv;
    if (!f->writable || dir->type != VFS::FT_DIR) return -1;
    uint32_t len = 0;
    if (!name_valid(name, &len)) return -1;

    uint64_t flags = irq_save();

    E2Node* d = (E2Node*)dir->priv;
    Inode din;
    if (!read_inode(f, d->ino, &din)) { irq_restore(flags); return -1; }

    uint32_t ino = 0;
    if (!dir_find(f, &din, name, len, &ino, nullptr, nullptr, nullptr)) {
        irq_restore(flags);
        return -1;
    }

    Inode in;
    if (!read_inode(f, ino, &in)) { irq_restore(flags); return -1; }
    bool is_dir = (in.mode & S_IFDIR_BIT) != 0;

    // A directory only goes when it is empty, and only through its own entry,
    // never through "." or ".." (name_valid already refused those).
    if (is_dir && !dir_is_empty(f, &in)) { irq_restore(flags); return -1; }

    if (!dir_remove(f, &din, name, len, nullptr, nullptr)) {
        irq_restore(flags);
        return -1;
    }

    uint32_t now = (uint32_t)Rtc::seconds_now();
    din.mtime = now;
    din.ctime = now;
    if (is_dir && din.links_count > 0) din.links_count--;   // the child's ".."
    din.blocks = blocks_to_512(f, count_blocks(f, &din));
    write_inode(f, d->ino, &din);
    refresh_node(d, dir, &din);

    in.links_count = 0;
    in.ctime = now;
    in.mtime = now;
    in.dtime = now ? now : 1;
    write_inode(f, ino, &in);

    inode_put(f, ino, is_dir);
    irq_restore(flags);
    return 0;
}

static int e2_rename(void* priv, VFS::Node* from_dir, const char* from,
                     VFS::Node* to_dir, const char* to) {
    Fs* f = (Fs*)priv;
    if (!f->writable || from_dir->type != VFS::FT_DIR || to_dir->type != VFS::FT_DIR) {
        return -1;
    }

    uint32_t flen = 0, tlen = 0;
    if (!name_valid(from, &flen) || !name_valid(to, &tlen)) {
        return -1;
    }
    if (flen == 1 && from[0] == '.') {
        return -1;
    }

    uint64_t flags = irq_save();

    E2Node* fdn = (E2Node*)from_dir->priv;
    E2Node* tdn = (E2Node*)to_dir->priv;
    Inode fdin, tdin;
    if (!read_inode(f, fdn->ino, &fdin) || !read_inode(f, tdn->ino, &tdin)) {
        irq_restore(flags);
        return -1;
    }
    bool same_dir = fdn->ino == tdn->ino;
    // When both names live in one directory, a single inode object is the truth:
    // two copies would let the insertion and the removal write each other's
    // changes away.
    Inode* tdir = same_dir ? &fdin : &tdin;

    uint32_t src_ino = 0;
    if (!dir_find(f, &fdin, from, flen, &src_ino, nullptr, nullptr, nullptr)) {
        irq_restore(flags);
        return -1;
    }
    if (same_dir && flen == tlen && name_eq(from, to, flen)) {
        irq_restore(flags);
        return 0;   // the same name in the same place: nothing to do
    }

    Inode src;
    if (!read_inode(f, src_ino, &src)) { irq_restore(flags); return -1; }
    bool src_is_dir = (src.mode & S_IFDIR_BIT) != 0;

    // A directory cannot be moved into its own subtree: walking up from the new
    // parent through ".." must not pass through the source. This catches both
    // "move into itself" and "move into a child", either of which would leave a
    // cycle no later path walk could escape.
    if (src_is_dir) {
        uint32_t p = tdn->ino;
        for (uint32_t depth = 0; depth < 4096; depth++) {
            if (p == src_ino) { irq_restore(flags); return -1; }
            if (p == f->root_ino) break;
            Inode pin;
            if (!read_inode(f, p, &pin)) break;
            uint32_t up = 0;
            if (!dotdot_ino(f, &pin, &up) || up == p) break;
            p = up;
        }
    }

    uint32_t dst_ino = 0;
    bool dst_exists = dir_find(f, tdir, to, tlen, &dst_ino, nullptr, nullptr, nullptr);
    if (dst_exists) {
        if (dst_ino == src_ino) { irq_restore(flags); return 0; }   // same inode
        Inode dst;
        if (!read_inode(f, dst_ino, &dst)) { irq_restore(flags); return -1; }
        bool dst_is_dir = (dst.mode & S_IFDIR_BIT) != 0;
        if (src_is_dir != dst_is_dir) { irq_restore(flags); return -1; }
        if (dst_is_dir && !dir_is_empty(f, &dst)) { irq_restore(flags); return -1; }

        if (!dir_remove(f, tdir, to, tlen, nullptr, nullptr)) {
            irq_restore(flags);
            return -1;
        }
        if (dst_is_dir) {
            // The replaced directory's ".." was one of this parent's links.
            if (tdir->links_count > 0) tdir->links_count--;
        }
        uint32_t now = (uint32_t)Rtc::seconds_now();
        dst.links_count = 0;
        dst.ctime = now;
        dst.mtime = now;
        dst.dtime = now ? now : 1;
        write_inode(f, dst_ino, &dst);
        inode_put(f, dst_ino, dst_is_dir);
    }

    // Link accounting for a directory that changes parent. The moved directory
    // gives up its old ".." and gains the new one.
    if (src_is_dir && !same_dir) {
        if (fdin.links_count > 0) fdin.links_count--;
        tdin.links_count++;
        if (!set_dotdot(f, &src, tdn->ino)) {
            irq_restore(flags);
            return -1;
        }
    }

    // Insert first, then remove: a failure halfway leaves the name reachable in
    // the old place rather than nowhere.
    uint8_t ftype = f->dev->has_filetype ? (src_is_dir ? 2 : 1) : 0;
    if (!dir_insert(f, tdir, to, tlen, src_ino, ftype)) {
        irq_restore(flags);
        return -1;
    }
    if (!dir_remove(f, &fdin, from, flen, nullptr, nullptr)) {
        irq_restore(flags);
        return -1;
    }

    uint32_t now = (uint32_t)Rtc::seconds_now();
    src.ctime = now;
    write_inode(f, src_ino, &src);

    fdin.mtime = now;
    fdin.ctime = now;
    fdin.blocks = blocks_to_512(f, count_blocks(f, &fdin));
    write_inode(f, fdn->ino, &fdin);
    refresh_node(fdn, from_dir, &fdin);

    if (!same_dir) {
        tdin.mtime = now;
        tdin.ctime = now;
        tdin.blocks = blocks_to_512(f, count_blocks(f, &tdin));
        write_inode(f, tdn->ino, &tdin);
        refresh_node(tdn, to_dir, &tdin);
    }

    irq_restore(flags);
    return 0;
}

static void e2_release(void* priv, VFS::Node* node) {
    Fs* f = (Fs*)priv;
    E2Node* n = (E2Node*)node->priv;
    if (n != nullptr) {
        uint64_t flags = irq_save();
        track_release(f, n->ino);   // the last reference may free an orphan
        irq_restore(flags);
        delete n;
    }
    delete node;
}

// The medium's size is in the superblock, and it is worth reporting: it is the
// one thing here that came off the disk rather than out of the kernel. The free
// count is the superblock's own, kept current as blocks are handed out.
static int e2_statfs(void* priv, VFS::StatFs* out) {
    Fs* f = (Fs*)priv;
    out->total = (uint64_t)f->dev->blocks_count * f->dev->block_size;
    out->free = f->writable
                    ? (uint64_t)f->dev->free_blocks * f->dev->block_size
                    : 0;
    out->writable = f->writable;
    out->type = "ext2";
    return 0;
}

// There is no cache between this and the device, so "sync" is the device's own
// flush plus the superblock's write time. The counts are written as they change.
static int e2_sync(void* priv) {
    Fs* f = (Fs*)priv;
    if (f->writable) {
        uint32_t now = (uint32_t)Rtc::seconds_now();
        write_bytes(f, 1024 + 48, &now, sizeof(now));
        save_counts(f);
    }
    return Block::flush(f->dev->dev) ? 0 : -1;
}

static VFS::Filesystem make_fs(bool writable) {
    VFS::Filesystem fs = {
        .fs_type  = "ext2",
        .priv     = nullptr,
        .root     = e2_root,
        .lookup   = e2_lookup,
        .readdir  = e2_readdir,
        .read     = e2_read,
        .write    = writable ? e2_write : nullptr,
        .truncate = writable ? e2_truncate : nullptr,
        .create   = writable ? e2_create : nullptr,
        .unlink   = writable ? e2_unlink : nullptr,
        .rename   = writable ? e2_rename : nullptr,
        .release  = e2_release,
        .statfs   = e2_statfs,
        .map      = nullptr,    // the RAM disk could, but nothing promised it
        .sync     = e2_sync,
    };
    return fs;
}

// --- mount ---

// One device entry per Block device, however many mounts ask for it. Mounting
// the same disk twice must not produce two sets of free counts.
static Dev* devices = nullptr;

static Dev* dev_find(Block::Device* dev) {
    for (Dev* d = devices; d; d = d->next)
        if (d->dev == dev) return d;
    return nullptr;
}

VFS::Filesystem* mount(Block::Device* dev, bool writable, uint32_t root_ino) {
    if (dev == nullptr) return nullptr;

    Dev* d = dev_find(dev);
    if (d == nullptr) {
        Dev probe;
        memset(&probe, 0, sizeof(probe));
        probe.dev = dev;
        Fs pf;
        pf.dev = &probe;
        pf.vfs = nullptr;
        pf.root_ino = 2;
        pf.writable = false;

        // The superblock is always at byte 1024 — before the block size is
        // known, so this read cannot go through the block helpers.
        Superblock sb;
        if (!read_bytes(&pf, 1024, &sb, sizeof(sb)) || sb.magic != MAGIC)
            return nullptr;
        if (sb.rev_level >= 1 && (sb.feature_incompat & INCOMPAT_64BIT))
            return nullptr;

        uint32_t bs = 1024u << sb.log_block_size;
        if (bs < 512 || bs > MAX_BLOCK || sb.blocks_per_group == 0 ||
            sb.inodes_per_group == 0 || sb.blocks_count == 0)
            return nullptr;

        probe.block_size = bs;
        probe.first_data_block = sb.first_data_block;
        probe.blocks_per_group = sb.blocks_per_group;
        probe.inodes_per_group = sb.inodes_per_group;
        probe.inodes_count = sb.inodes_count;
        probe.blocks_count = sb.blocks_count;
        probe.inode_size = (sb.rev_level >= 1 && sb.inode_size >= sizeof(Inode))
                               ? sb.inode_size : sizeof(Inode);
        probe.groups = (sb.blocks_count + sb.blocks_per_group - 1) /
                       sb.blocks_per_group;
        probe.free_blocks = sb.free_blocks_count;
        probe.free_inodes = sb.free_inodes_count;
        probe.feature_incompat = sb.rev_level >= 1 ? sb.feature_incompat : 0;
        probe.has_filetype = (probe.feature_incompat & INCOMPAT_FILETYPE) != 0;
        probe.tracked = nullptr;
        probe.next = nullptr;

        d = new Dev;
        if (d == nullptr) return nullptr;
        *d = probe;
        d->next = devices;
        devices = d;
    }

    // Writable mounts refuse features whose on-disk structures this writer does
    // not maintain (extents, a journal, 64-bit block numbers).
    if (writable &&
        (d->feature_incompat & (INCOMPAT_EXTENTS | INCOMPAT_RECOVER |
                                INCOMPAT_64BIT)))
        return nullptr;

    Fs* f = new Fs;
    if (f == nullptr) return nullptr;
    f->dev = d;
    f->root_ino = root_ino;
    f->writable = writable;

    Inode root;
    if (!read_inode(f, root_ino, &root) || (root.mode & S_IFDIR_BIT) == 0) {
        delete f;
        return nullptr;
    }

    VFS::Filesystem* vfs = new VFS::Filesystem;
    if (vfs == nullptr) { delete f; return nullptr; }
    *vfs = make_fs(writable);
    vfs->priv = f;
    f->vfs = vfs;

    kprint("[Ext2] ");
    kprint_uint(d->block_size);
    kprint("-byte blocks, ");
    kprint_uint(d->blocks_count);
    kprint(" blocks, ");
    kprint_uint(d->inodes_count);
    kprint(" inodes, ");
    kprint_uint(d->groups);
    kprint(d->groups == 1
               ? (writable ? " group (read-write)\n" : " group (read-only)\n")
               : (writable ? " groups (read-write)\n" : " groups (read-only)\n"));
    return vfs;
}

} // namespace Ext2
