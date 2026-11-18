// test_ext2_rename: the rename state machine, driven over an ext2 image in
// memory with a write fault where a guest cannot have one.
//
// Why a host test and not a guest scenario: ext2 writes through BlockCache, so
// a directory block that was just searched is a cache hit, and a cached write
// never reaches the device. The last directory removal of a rename therefore
// cannot be made to fail from outside the filesystem; the fault is injected
// inside write_block, one nominated block at a time (src/kernel/dev/ext2.h,
// EXT2_HOST_TEST), and the whole translation unit runs natively with the
// interrupt lock and the console stubbed out below.
//
// The image is the one tools/mkext2.py builds for the shipped disk: a real
// mke2fs image with hello.txt, sub/nested.txt, big.bin and root/ in it. Nothing
// is written back to the file; the fake device owns the bytes.
//
// What it pins: after a rename whose last step fails, the image holds one
// coherent state, not two names sharing one link count.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdint.h>

#include "fs/vfs.h"
#include "kernel/core/block.h"
#include "kernel/core/blockcache.h"
#include "kernel/core/errno.h"
#include "kernel/dev/ext2.h"

// --- the console, the clock and the node allocator, stubbed ---
//
// These are the only things ext2.cpp asks of the kernel that are not part of
// the block device contract.

extern "C" void kprint(const char*) {}
extern "C" void kprint_uint(uint64_t) {}
extern "C" void kprint_hex(uint64_t) {}
extern "C" void kprint3(const char*, const char*, const char*) {}
extern "C" void kprint_lus(const char*, uint64_t, const char*) {}

namespace Rtc {
uint64_t boot_epoch() { return 1700000000; }
uint64_t seconds_now() { return 1700000000; }
} // namespace Rtc

namespace VFS {
Node* node_new(Filesystem* fs, FileType type) {
    Node* n = new Node;
    n->fs = fs;
    n->priv = nullptr;
    n->type = type;
    n->size = 0;
    n->ino = 0;
    n->mode = 0;
    n->mtime = 0;
    return n;
}
} // namespace VFS

// --- a device over a byte buffer ---

struct Disk {
    uint8_t* bytes;
    uint64_t sectors;
};

static uint64_t disk_read(void* priv, uint64_t lba, uint64_t count, void* buf) {
    Disk* d = (Disk*)priv;
    if (lba > d->sectors || count > d->sectors - lba) return 0;
    memcpy(buf, d->bytes + lba * Block::SECTOR, count * Block::SECTOR);
    return count;
}

static uint64_t disk_write(void* priv, uint64_t lba, uint64_t count,
                           const void* buf) {
    Disk* d = (Disk*)priv;
    if (lba > d->sectors || count > d->sectors - lba) return 0;
    memcpy(d->bytes + lba * Block::SECTOR, buf, count * Block::SECTOR);
    return count;
}

static bool disk_flush(void*) { return true; }

// --- the on-disk shape, read straight out of the image ---
//
// Deliberately not through ext2: the test is checking the bytes that landed on
// the "disk", so it reads them the way e2fsck would.

struct Meta {
    uint32_t bs;
    uint32_t inode_size;
    uint32_t inodes_per_group;
    uint32_t first_data_block;
    uint32_t inode_table;
};

static uint32_t u16_at(const uint8_t* p, uint64_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8);
}

static uint32_t u32_at(const uint8_t* p, uint64_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
           ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

static Meta meta_of(const uint8_t* img) {
    const uint8_t* sb = img + 1024;
    Meta m;
    m.first_data_block = u32_at(sb, 20);
    m.bs = 1024u << u32_at(sb, 24);
    m.inodes_per_group = u32_at(sb, 40);
    m.inode_size = u32_at(sb, 88);
    if (m.inode_size < 128) m.inode_size = 128;   // rev 0 images
    m.inode_table =
        u32_at(img, (uint64_t)(m.first_data_block + 1) * m.bs + 8);
    return m;
}

// The shipped image has one block group, so the inode table block in the first
// group descriptor is the table for every inode in it.
static uint64_t inode_off(const Meta& m, uint32_t ino) {
    uint32_t index = (ino - 1) % m.inodes_per_group;
    return (uint64_t)m.inode_table * m.bs + (uint64_t)index * m.inode_size;
}

static uint32_t inode_links(const uint8_t* img, const Meta& m, uint32_t ino) {
    return u16_at(img, inode_off(m, ino) + 26);
}

// The block that actually holds 'name', scanning the direct pointers the way
// the directory code does. A test that guessed block zero would pass while
// proving nothing once a directory is big enough to have a second block.
static uint32_t block_holding(const uint8_t* img, const Meta& m, uint32_t dir_ino,
                              const char* name) {
    uint64_t off = inode_off(m, dir_ino);
    uint32_t size = u32_at(img, off + 4);
    uint32_t len = (uint32_t)strlen(name);
    uint32_t nblocks = (size + m.bs - 1) / m.bs;

    for (uint32_t b = 0; b < nblocks && b < 12; b++) {
        uint32_t blk = u32_at(img, off + 40 + b * 4);
        if (blk == 0) continue;
        const uint8_t* raw = img + (uint64_t)blk * m.bs;
        uint32_t p = 0;
        while (p + 8 <= m.bs) {
            uint32_t rec = u16_at(raw, p + 4);
            uint32_t ino = u32_at(raw, p);
            if (rec < 8 || p + rec > m.bs) break;
            if (ino != 0 && raw[p + 6] == len &&
                memcmp(raw + p + 8, name, len) == 0)
                return blk;
            p += rec;
        }
    }
    return 0;
}

// The inode a directory's ".." names, read out of its first block.
static uint32_t dotdot_of(const uint8_t* img, const Meta& m, uint32_t dir_ino) {
    uint64_t off = inode_off(m, dir_ino);
    uint32_t blk = u32_at(img, off + 40);
    if (blk == 0) return 0;
    const uint8_t* raw = img + (uint64_t)blk * m.bs;
    uint32_t p = 0;
    while (p + 8 <= m.bs) {
        uint32_t rec = u16_at(raw, p + 4);
        if (rec < 8 || p + rec > m.bs) break;
        if (raw[p + 6] == 2 && memcmp(raw + p + 8, "..", 2) == 0)
            return u32_at(raw, p);
        p += rec;
    }
    return 0;
}

static uint32_t inode_dtime(const uint8_t* img, const Meta& m, uint32_t ino) {
    return u32_at(img, inode_off(m, ino) + 20);
}

// Every entry called 'name' in a directory, counted across the whole block the
// way e2fsck would. A lookup stops at the first match, so it cannot see a
// duplicate entry, and a duplicate is one of the two outcomes this test is
// built to catch.
struct NameScan {
    int count;       // entries with this name
    uint32_t ino;    // the inode the first of them names
};

static NameScan scan_name(const uint8_t* img, const Meta& m, uint32_t dir_ino,
                          const char* name) {
    NameScan s;
    s.count = 0;
    s.ino = 0;
    uint64_t off = inode_off(m, dir_ino);
    uint32_t size = u32_at(img, off + 4);
    uint32_t len = (uint32_t)strlen(name);
    uint32_t nblocks = (size + m.bs - 1) / m.bs;

    for (uint32_t b = 0; b < nblocks && b < 12; b++) {
        uint32_t blk = u32_at(img, off + 40 + b * 4);
        if (blk == 0) continue;
        const uint8_t* raw = img + (uint64_t)blk * m.bs;
        uint32_t p = 0;
        while (p + 8 <= m.bs) {
            uint32_t rec = u16_at(raw, p + 4);
            uint32_t ino = u32_at(raw, p);
            if (rec < 8 || p + rec > m.bs) break;
            if (ino != 0 && raw[p + 6] == len &&
                memcmp(raw + p + 8, name, len) == 0) {
                s.count++;
                if (s.ino == 0) s.ino = ino;
            }
            p += rec;
        }
    }
    return s;
}

// --- checks ---

static int checks;
static int failures;

static void check(bool cond, const char* what) {
    checks++;
    if (cond) return;
    failures++;
    printf("FAIL %s\n", what);
}

struct Case {
    uint8_t* bytes;
    uint64_t sectors;
    Disk disk;
    Block::Device dev;
};

// Clear every fault, so a case cannot inherit one from the case before it.
static void disarm() {
    for (int i = 0; i < Ext2::FAULT_SLOTS; i++) {
        Ext2::host_fail_write_block[i] = 0;
        Ext2::host_fail_write_skip[i] = 0;
    }
}

static bool load(const char* path, Case& c) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        printf("FAIL cannot open %s\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    c.bytes = (uint8_t*)calloc(1, (size_t)n);
    if (c.bytes == nullptr || fread(c.bytes, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        printf("FAIL cannot read %s\n", path);
        return false;
    }
    fclose(f);
    c.sectors = (uint64_t)n / Block::SECTOR;
    c.disk.bytes = c.bytes;
    c.disk.sectors = c.sectors;
    c.dev.name = "rename-test";
    c.dev.sectors = c.sectors;
    c.dev.read = disk_read;
    c.dev.write = disk_write;
    c.dev.flush = disk_flush;
    c.dev.priv = &c.disk;
    return true;
}

// Every case gets its own Block::Device, so the page cache keys it separately
// and no page from an earlier case can be mistaken for a hit.
static VFS::Filesystem* mount_case(Case& c, const char* path) {
    if (!load(path, c)) return nullptr;
    return Ext2::mount(&c.dev, true);
}

// The filesystem writes through a write-back page cache, so the image bytes
// only change once a dirty page is written out. Every check that reads the
// image directly syncs first; the checks that go through the backend do not
// need to.
static void sync_disk(Case& c) { BlockCache::flush(&c.dev); }

static VFS::Node* look(VFS::Filesystem* fs, VFS::Node* dir, const char* name) {
    return fs->lookup(fs->priv, dir, name);
}

static bool exists(VFS::Filesystem* fs, VFS::Node* dir, const char* name) {
    VFS::Node* n = look(fs, dir, name);
    if (n == nullptr) return false;
    fs->release(fs->priv, n);
    return true;
}

int main(int argc, char** argv) {
    // One image per case, and each at its own address: the page cache keys its
    // pages by device pointer, so two cases sharing a stack slot would read one
    // image's cached pages while looking at another's bytes.
    static Case cases[5];

    if (argc < 2) {
        printf("usage: %s <ext2 image>\n", argv[0]);
        return 2;
    }

    // --- A: the last removal fails, so the rename is undone ---
    //
    // hello.txt lives in the root, sub/ is a directory of its own: a file
    // moving between two directories, which is the case where ".." is not in
    // play and the undo is the plain one.
    {
        Case& c = cases[0];
        VFS::Filesystem* fs = mount_case(c, argv[1]);
        check(fs != nullptr, "A: mount");
        if (fs != nullptr) {
            Meta m = meta_of(c.bytes);
            VFS::Node* root = fs->root(fs->priv);
            VFS::Node* sub = look(fs, root, "sub");
            VFS::Node* hello = look(fs, root, "hello.txt");
            check(sub != nullptr && hello != nullptr, "A: the image has hello.txt and sub/");
            if (sub != nullptr && hello != nullptr) {
                uint32_t root_ino = root->ino;
                uint32_t sub_ino = sub->ino;
                uint32_t hello_ino = hello->ino;
                uint32_t root_links = inode_links(c.bytes, m, root_ino);
                uint32_t sub_links = inode_links(c.bytes, m, sub_ino);
                uint32_t from_blk = block_holding(c.bytes, m, root_ino, "hello.txt");
                disarm();
                check(from_blk != 0, "A: found the block holding hello.txt");
                fs->release(fs->priv, hello);

                // Fail the write that would take the old name out.
                Ext2::host_fail_write_block[0] = (int)from_blk;
                int rc = fs->rename(fs->priv, root, "hello.txt", sub, "moved.txt");
                check(rc == -Errno::EIO, "A: rename reports EIO");
                check(Ext2::host_fail_write_block[0] == 0, "A: the fault was taken");
                check(exists(fs, root, "hello.txt"), "A: the old name is still there");
                check(!exists(fs, sub, "moved.txt"), "A: the new name was taken back out");
                sync_disk(c);
                check(inode_links(c.bytes, m, hello_ino) == 1,
                      "A: the file still has exactly one link");
                check(inode_links(c.bytes, m, root_ino) == root_links,
                      "A: the old parent's link count is unchanged");
                check(inode_links(c.bytes, m, sub_ino) == sub_links,
                      "A: the new parent's link count is unchanged");

                // And the filesystem is still usable, which is the point of
                // rolling back rather than half committing.
                check(fs->rename(fs->priv, root, "hello.txt", sub, "moved.txt") == 0,
                      "A: the same rename succeeds once the block takes writes");
                check(!exists(fs, root, "hello.txt"), "A: the old name is gone");
                check(exists(fs, sub, "moved.txt"), "A: the new name is there");
                sync_disk(c);
                check(inode_links(c.bytes, m, hello_ino) == 1,
                      "A: a moved file has one link");
            }
            fs->release(fs->priv, sub);
            fs->release(fs->priv, root);
        }
    }

    // --- B: both removals fail, the third state is the documented one ---
    //
    // The destination name cannot be taken out either, so the rename cannot be
    // undone. Two names are left, and the inode is given the link the second
    // name needs: unlinking either name must then free the file exactly once.
    {
        Case& c = cases[1];
        VFS::Filesystem* fs = mount_case(c, argv[1]);
        check(fs != nullptr, "B: mount");
        if (fs != nullptr) {
            Meta m = meta_of(c.bytes);
            VFS::Node* root = fs->root(fs->priv);
            VFS::Node* sub = look(fs, root, "sub");
            VFS::Node* hello = look(fs, root, "hello.txt");
            if (sub != nullptr && hello != nullptr) {
                uint32_t root_ino = root->ino;
                uint32_t sub_ino = sub->ino;
                uint32_t hello_ino = hello->ino;
                fs->release(fs->priv, hello);

                disarm();
                // Insertion into 'sub' first, which must succeed, then the
                // removal from the old parent, then the removal from the new
                // one: the block of 'sub' is written twice, and only the
                // second write is nominated to fail.
                Ext2::host_fail_write_block[0] =
                    (int)block_holding(c.bytes, m, root_ino, "hello.txt");
                Ext2::host_fail_write_block[1] =
                    (int)block_holding(c.bytes, m, sub_ino, "nested.txt");
                Ext2::host_fail_write_skip[1] = 1;
                check(Ext2::host_fail_write_block[0] != 0 &&
                          Ext2::host_fail_write_block[1] != 0,
                      "B: found both directory blocks");
                int rc = fs->rename(fs->priv, root, "hello.txt", sub, "moved.txt");
                check(rc == -Errno::EIO, "B: rename reports EIO");
                check(exists(fs, root, "hello.txt"), "B: the old name survived");
                check(exists(fs, sub, "moved.txt"), "B: the new name survived");
                sync_disk(c);
                check(inode_links(c.bytes, m, hello_ino) == 2,
                      "B: the file carries a link for each name");

                // Either name can now be freed without killing the other.
                check(fs->unlink(fs->priv, sub, "moved.txt") == 0, "B: unlink one name");
                sync_disk(c);
                check(inode_links(c.bytes, m, hello_ino) == 1,
                      "B: the remaining name still has its link");
                VFS::Node* again = look(fs, root, "hello.txt");
                check(again != nullptr, "B: the remaining name still resolves");
                if (again != nullptr) {
                    char buf[64];
                    uint64_t n = fs->read(fs->priv, again, buf, sizeof(buf), 0);
                    check(n > 0, "B: the remaining name still reads");
                    fs->release(fs->priv, again);
                }
            }
            fs->release(fs->priv, sub);
            fs->release(fs->priv, root);
        }
    }

    // --- C: control. A rename over an existing name, with no fault, still
    // replaces it and frees the replaced inode.
    {
        Case& c = cases[2];
        VFS::Filesystem* fs = mount_case(c, argv[1]);
        check(fs != nullptr, "C: mount");
        if (fs != nullptr) {
            Meta m = meta_of(c.bytes);
            VFS::Node* root = fs->root(fs->priv);
            VFS::Node* sub = look(fs, root, "sub");
            VFS::Node* nested = look(fs, sub, "nested.txt");
            check(nested != nullptr, "C: the image has sub/nested.txt");
            uint32_t nested_ino = nested ? nested->ino : 0;
            if (nested) fs->release(fs->priv, nested);
            disarm();
            check(fs->rename(fs->priv, root, "hello.txt", sub, "nested.txt") == 0,
                  "C: the rename succeeds");
            check(!exists(fs, root, "hello.txt"), "C: the source name is gone");
            check(exists(fs, sub, "nested.txt"), "C: the destination name is there");
            sync_disk(c);
            if (nested_ino != 0)
                check(inode_links(c.bytes, m, nested_ino) == 0,
                      "C: the replaced inode was given up");
            fs->release(fs->priv, sub);
            fs->release(fs->priv, root);
        }
    }

    // --- D: a directory moved to another parent, undone ---
    //
    // The case with the most to put back: the new ".." has already been
    // written and both parents' link counts have moved, so the same failed last
    // removal has to undo all three.
    {
        Case& c = cases[3];
        VFS::Filesystem* fs = mount_case(c, argv[1]);
        check(fs != nullptr, "D: mount");
        if (fs != nullptr) {
            Meta m = meta_of(c.bytes);
            VFS::Node* top = fs->root(fs->priv);
            VFS::Node* sub = look(fs, top, "sub");
            VFS::Node* upper = look(fs, top, "root");
            check(sub != nullptr && upper != nullptr,
                  "D: the image has sub/ and root/");
            if (sub != nullptr && upper != nullptr) {
                uint32_t top_ino = top->ino;
                uint32_t sub_ino = sub->ino;
                uint32_t upper_ino = upper->ino;
                uint32_t top_links = inode_links(c.bytes, m, top_ino);
                uint32_t upper_links = inode_links(c.bytes, m, upper_ino);
                uint32_t from_blk = block_holding(c.bytes, m, top_ino, "sub");
                disarm();
                check(from_blk != 0, "D: found the block holding sub");
                Ext2::host_fail_write_block[0] = (int)from_blk;

                int rc = fs->rename(fs->priv, top, "sub", upper, "sub2");
                check(rc == -Errno::EIO, "D: rename reports EIO");
                check(exists(fs, top, "sub"), "D: the directory is back in the old parent");
                check(!exists(fs, upper, "sub2"), "D: the new name was taken back out");
                sync_disk(c);
                check(inode_links(c.bytes, m, top_ino) == top_links,
                      "D: the old parent's link count is back");
                check(inode_links(c.bytes, m, upper_ino) == upper_links,
                      "D: the new parent's link count is back");
                check(dotdot_of(c.bytes, m, sub_ino) == top_ino,
                      "D: \"..\" names the old parent again");
            }
            if (sub != nullptr) fs->release(fs->priv, sub);
            if (upper != nullptr) fs->release(fs->priv, upper);
            fs->release(fs->priv, top);
        }
    }

    // --- E: both removals fail over an existing destination ---
    //
    // The third state again, this time with a name to replace. 'moved.txt' is
    // still there and now names the source, and the inode it used to name has
    // no name left anywhere. Putting that entry back would either duplicate the
    // name (dir_reinsert only looks for a free record, never for a live entry
    // with the same name) or orphan the inode when no free record is big
    // enough. It is freed instead, which is the only state nothing can be
    // trapped behind.
    {
        Case& c = cases[4];
        VFS::Filesystem* fs = mount_case(c, argv[1]);
        check(fs != nullptr, "E: mount");
        if (fs != nullptr) {
            Meta m = meta_of(c.bytes);
            VFS::Node* root = fs->root(fs->priv);
            VFS::Node* sub = look(fs, root, "sub");
            VFS::Node* hello = look(fs, root, "hello.txt");
            check(sub != nullptr && hello != nullptr,
                  "E: the image has hello.txt and sub/");
            if (sub != nullptr && hello != nullptr) {
                uint32_t root_ino = root->ino;
                uint32_t sub_ino = sub->ino;
                uint32_t hello_ino = hello->ino;

                char source[128];
                memset(source, 0, sizeof(source));
                uint64_t source_len =
                    fs->read(fs->priv, hello, source, sizeof(source) - 1, 0);
                check(source_len > 0, "E: hello.txt reads");

                // The destination has to exist before the rename, with content
                // of its own, so "the name now reads the source" says something
                // about which file the surviving entry reaches.
                static const char replaced_text[] = "the replaced file, not the source";
                VFS::Node* dst = nullptr;
                int crc = fs->create(fs->priv, sub, "moved.txt", VFS::FT_FILE, &dst);
                check(crc == 0 && dst != nullptr, "E: created sub/moved.txt");
                uint32_t dst_ino = dst ? dst->ino : 0;
                if (dst != nullptr) {
                    uint64_t n = fs->write(fs->priv, dst, replaced_text,
                                           sizeof(replaced_text) - 1, 0);
                    check(n == sizeof(replaced_text) - 1,
                          "E: the replaced file has content");
                    fs->release(fs->priv, dst);
                }
                fs->release(fs->priv, hello);

                if (dst_ino != 0) {
                    sync_disk(c);
                    uint32_t root_blk = block_holding(c.bytes, m, root_ino, "hello.txt");
                    uint32_t sub_blk = block_holding(c.bytes, m, sub_ino, "moved.txt");
                    check(inode_links(c.bytes, m, dst_ino) == 1,
                          "E: the replaced file starts with one link");
                    disarm();
                    check(root_blk != 0 && sub_blk != 0 && root_blk != sub_blk,
                          "E: found both directory blocks");

                    // The old parent's removal fails, and so does the removal
                    // from the new parent that would take the new name back
                    // out. The replaced name's removal and the insertion after
                    // it are let through, or there would be nothing to restore
                    // and the case would not be the one under test.
                    Ext2::host_fail_write_block[0] = (int)root_blk;
                    Ext2::host_fail_write_block[1] = (int)sub_blk;
                    Ext2::host_fail_write_skip[1] = 2;

                    int rc = fs->rename(fs->priv, root, "hello.txt", sub, "moved.txt");
                    check(rc == -Errno::EIO, "E: rename reports EIO");
                    check(Ext2::host_fail_write_block[0] == 0 &&
                              Ext2::host_fail_write_block[1] == 0,
                          "E: both faults were taken");
                    check(exists(fs, root, "hello.txt"), "E: the old name survived");

                    sync_disk(c);
                    NameScan got = scan_name(c.bytes, m, sub_ino, "moved.txt");
                    check(got.count == 1, "E: exactly one entry is called moved.txt");
                    check(got.ino == hello_ino, "E: that entry names the source inode");
                    check(inode_links(c.bytes, m, dst_ino) == 0,
                          "E: the replaced inode was given up");
                    check(inode_dtime(c.bytes, m, dst_ino) != 0,
                          "E: the replaced inode has a deletion time");

                    VFS::Node* moved = look(fs, sub, "moved.txt");
                    check(moved != nullptr, "E: moved.txt resolves");
                    if (moved != nullptr) {
                        char back[128];
                        memset(back, 0, sizeof(back));
                        uint64_t n = fs->read(fs->priv, moved, back,
                                              sizeof(back) - 1, 0);
                        check(n == source_len && memcmp(back, source, n) == 0,
                              "E: moved.txt reads hello.txt's content");
                        fs->release(fs->priv, moved);
                    }
                }
            }
            if (sub != nullptr) fs->release(fs->priv, sub);
            fs->release(fs->priv, root);
        }
    }

    for (int i = 0; i < Ext2::FAULT_SLOTS; i++) {
        check(Ext2::host_fail_write_block[i] == 0, "no fault left armed");
        check(Ext2::host_fail_write_skip[i] == 0, "no fault skip left armed");
    }

    if (failures) {
        printf("test_ext2_rename: %d/%d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_ext2_rename: %d checks passed\n", checks);
    return 0;
}
