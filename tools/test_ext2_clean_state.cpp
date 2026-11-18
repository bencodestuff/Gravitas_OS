// test_ext2_clean_state: the ext2 clean/dirty flag, proved on the device.
//
// Why a host test: the promise is about the order the *medium* sees writes in,
// and no guest can show that. A guest sees the page cache, so a block that was
// just written answers the next read without reaching a drive, and a power cut
// after a sync and one before it look the same from inside. Here the device is
// a byte buffer that records every write and every flush, so "the superblock
// was on the medium before the data" is a fact about the buffer, not a hope.
//
// The image is the one tools/mkext2.py builds for the shipped disk. Each case
// gets its own copy and its own Block::Device, because the page cache keys
// pages by device pointer.
//
// What it pins:
//   - a mount writes nothing, so a fresh image stays clean until written;
//   - the first write clears EXT2_VALID_FS and flushes the superblock before
//     any write outside the page that holds it;
//   - sync writes the flag back, and the next write clears it again;
//   - a dirty image is refused read-write with EIO, untouched;
//   - a second mount of a disk this boot made dirty is still allowed, which is
//     what the two mounts of the data disk at boot need.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "fs/vfs.h"
#include "kernel/core/block.h"
#include "kernel/core/blockcache.h"
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

// --- the device's view of the superblock, read the way e2fsck would ---

static uint32_t u16_at(const uint8_t* p, uint64_t off) {
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8);
}

// --- a device over a byte buffer that remembers what it was told ---

struct Disk {
    uint8_t* bytes;
    uint64_t sectors;
    uint32_t writes;                    // device writes, in any sector
    uint32_t writebacks;                // flush() calls
    uint32_t early_writes;              // writes past page 0 before the flag landed
    bool flag_on_medium;                // a flush carried s_state with VALID_FS clear
};

static uint64_t disk_read(void* priv, uint64_t lba, uint64_t count, void* buf) {
    Disk* d = (Disk*)priv;
    if (lba > d->sectors || count > d->sectors - lba) return 0;
    std::memcpy(buf, d->bytes + lba * Block::SECTOR, count * Block::SECTOR);
    return count;
}

static uint64_t disk_write(void* priv, uint64_t lba, uint64_t count,
                           const void* buf) {
    Disk* d = (Disk*)priv;
    if (lba > d->sectors || count > d->sectors - lba) return 0;
    // The superblock and the group descriptors live in the first 4 KB page, so
    // any write past it is metadata or data the dirty flag is supposed to
    // cover. If one arrives before a flush has carried the flag, the promise
    // is broken and this count says so.
    if (lba * Block::SECTOR >= BlockCache::PAGE_SIZE && !d->flag_on_medium)
        d->early_writes++;
    std::memcpy(d->bytes + lba * Block::SECTOR, buf, count * Block::SECTOR);
    d->writes++;
    return count;
}

static bool disk_flush(void* priv) {
    Disk* d = (Disk*)priv;
    d->writebacks++;
    if ((u16_at(d->bytes, 1024 + 58) & 0x0001) == 0) d->flag_on_medium = true;
    return true;
}

// --- the on-disk superblock, read the way e2fsck would ---

struct Case {
    uint8_t* bytes;
    uint64_t sectors;
    Disk disk;
    Block::Device dev;
};

static bool load(const char* path, Case& c) {
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) {
        std::printf("FAIL cannot open %s\n", path);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    c.bytes = (uint8_t*)std::calloc(1, (size_t)n);
    if (c.bytes == nullptr || std::fread(c.bytes, 1, (size_t)n, f) != (size_t)n) {
        std::fclose(f);
        std::printf("FAIL cannot read %s\n", path);
        return false;
    }
    std::fclose(f);
    c.sectors = (uint64_t)n / Block::SECTOR;
    c.disk.bytes = c.bytes;
    c.disk.sectors = c.sectors;
    c.disk.writes = 0;
    c.disk.writebacks = 0;
    c.disk.early_writes = 0;
    c.disk.flag_on_medium = false;
    c.dev.name = "clean-state-test";
    c.dev.sectors = c.sectors;
    c.dev.read = disk_read;
    c.dev.write = disk_write;
    c.dev.flush = disk_flush;
    c.dev.priv = &c.disk;
    return true;
}

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        std::printf("FAIL - %s\n", what);
        failures++;
    }
}

int main(int argc, char** argv) {
    // Static so each case keeps its own bytes and device for the whole run: the
    // page cache keys pages by device pointer, so two cases must never share
    // one, and the bytes must outlive the cache's view of them.
    static Case cases[3];

    if (argc < 2) {
        std::printf("usage: %s <ext2 image>\n", argv[0]);
        return 2;
    }

    // --- A: mount, first write, sync, write again ---
    {
        Case& c = cases[0];
        check(load(argv[1], c), "A: the image loads");
        VFS::Filesystem* fs = Ext2::mount(&c.dev, true);
        check(fs != nullptr, "A: a clean image mounts read-write");
        if (fs != nullptr) {
            check(c.disk.writes == 0, "A: mounting writes nothing");
            check(c.disk.writebacks == 0, "A: mounting flushes nothing");
            check((u16_at(c.bytes, 1024 + 58) & 0x0001) != 0,
                  "A: the image is still marked clean after mount");

            VFS::Node* root = fs->root(fs->priv);
            VFS::Node* hello = root ? fs->lookup(fs->priv, root, "hello.txt")
                                    : nullptr;
            check(hello != nullptr, "A: the image has hello.txt");
            if (hello != nullptr) {
                uint8_t buf[1024];
                std::memset(buf, 'A', sizeof(buf));
                uint64_t n = fs->write(fs->priv, hello, buf, sizeof(buf), 0);
                check(n == sizeof(buf), "A: the first write lands");
                check(c.disk.writes > 0, "A: the first write reached the device");
                check(c.disk.writebacks > 0,
                      "A: the dirty flag was flushed, not left in the cache");
                check(c.disk.early_writes == 0,
                      "A: nothing past the superblock page was written before "
                      "the flag reached the medium");
                check((u16_at(c.bytes, 1024 + 58) & 0x0001) == 0,
                      "A: the image is dirty on the medium after the first write");

                // Sync has to put it back, and the next write has to clear it
                // again: a flag that only ever clears is a flag that lies the
                // second time the machine is used.
                check(fs->sync(fs->priv) == 0, "A: sync succeeds");
                check((u16_at(c.bytes, 1024 + 58) & 0x0001) != 0,
                      "A: sync puts the clean flag back on the medium");
                c.disk.flag_on_medium = false;
                std::memset(buf, 'B', sizeof(buf));
                n = fs->write(fs->priv, hello, buf, sizeof(buf), 1024);
                check(n == sizeof(buf), "A: a write after sync lands");
                check(c.disk.early_writes == 0,
                      "A: that write also put the flag out first");
                check((u16_at(c.bytes, 1024 + 58) & 0x0001) == 0,
                      "A: a write after sync makes the image dirty again");
                fs->release(fs->priv, hello);
            }
            if (root != nullptr) fs->release(fs->priv, root);
        }
    }

    // --- B: a dirty image is refused read-write, and left alone ---
    {
        Case& c = cases[1];
        check(load(argv[1], c), "B: the image loads");
        c.bytes[1024 + 58] = 0;
        c.bytes[1024 + 59] = 0;
        const uint64_t size = c.sectors * Block::SECTOR;
        uint8_t* before = (uint8_t*)std::malloc(size);
        std::memcpy(before, c.bytes, size);

        VFS::Filesystem* fs = Ext2::mount(&c.dev, true);
        check(fs == nullptr, "B: a dirty image is refused read-write");
        check(c.disk.writes == 0, "B: a refused mount writes nothing");
        check(std::memcmp(before, c.bytes, size) == 0,
              "B: a refused mount leaves the image byte for byte");

        VFS::Filesystem* ro = Ext2::mount(&c.dev, false);
        check(ro != nullptr, "B: the same image still mounts read-only");
        check(c.disk.writes == 0, "B: the read-only mount writes nothing either");
        if (ro != nullptr) {
            VFS::Node* root = ro->root(ro->priv);
            check(root != nullptr, "B: the read-only mount has a root");
            if (root != nullptr) ro->release(ro->priv, root);
        }
        std::free(before);
    }

    // --- C: the boot's second mount of the same disk is not "dirty" ---
    //
    // The kernel mounts the data disk writable at inode 2 and then again at
    // root/ for the overlay's upper layer. If the first mount wrote anything in
    // between, the second would be refused by the check above and the writable
    // root would silently disappear.
    {
        Case& c = cases[2];
        check(load(argv[1], c), "C: the image loads");
        VFS::Filesystem* first = Ext2::mount(&c.dev, true);
        check(first != nullptr, "C: the first mount works");
        if (first != nullptr) {
            VFS::Node* root = first->root(first->priv);
            VFS::Node* hello = root ? first->lookup(first->priv, root, "hello.txt")
                                    : nullptr;
            if (hello != nullptr) {
                uint8_t buf[64];
                std::memset(buf, 'C', sizeof(buf));
                check(first->write(first->priv, hello, buf, sizeof(buf), 0) ==
                          sizeof(buf),
                      "C: the first mount writes");
                first->release(first->priv, hello);
            }
            if (root != nullptr) first->release(first->priv, root);

            VFS::Filesystem* second = Ext2::mount(&c.dev, true);
            check(second != nullptr,
                  "C: a second mount of a disk this boot marked is allowed");
            if (second != nullptr) {
                VFS::Node* root2 = second->root(second->priv);
                check(root2 != nullptr, "C: the second mount has a root");
                if (root2 != nullptr) second->release(second->priv, root2);
            }
        }
    }

    if (failures == 0) {
        std::printf("test_ext2_clean_state: all %d clean-state checks pass\n",
                    checks);
        return 0;
    }
    std::printf("test_ext2_clean_state: %d of %d checks failed\n", failures,
                checks);
    return 1;
}
