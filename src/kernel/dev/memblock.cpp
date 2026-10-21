#include "kernel/dev/memblock.h"
#include "kernel/core/heap.h"
#include "kernel/klib/klib.h"

// See memblock.h. One static Device per disk, and the bytes on the kernel heap:
// a RAM disk is just a buffer with a contract in front of it.

namespace MemBlock {

constexpr int MAX_DISKS = 2;

static uint8_t* store[MAX_DISKS];
static Block::Device devs[MAX_DISKS];
static int ndisks = 0;

static uint64_t in_range(Block::Device* d, uint64_t lba, uint64_t count) {
    if (lba >= d->sectors) return 0;
    if (count > d->sectors - lba) return 0; // runs past the end: refuse whole
    return count;
}

static uint64_t mb_read(void* priv, uint64_t lba, uint64_t count, void* buf) {
    Block::Device* d = (Block::Device*)priv;
    uint64_t n = in_range(d, lba, count);
    uint8_t* p = store[(int)(d - devs)];
    uint8_t* dst = (uint8_t*)buf;
    for (uint64_t i = 0; i < n * Block::SECTOR; i++)
        dst[i] = p[lba * Block::SECTOR + i];
    return n;
}

static uint64_t mb_write(void* priv, uint64_t lba, uint64_t count, const void* buf) {
    Block::Device* d = (Block::Device*)priv;
    uint64_t n = in_range(d, lba, count);
    uint8_t* p = store[(int)(d - devs)];
    const uint8_t* src = (const uint8_t*)buf;
    for (uint64_t i = 0; i < n * Block::SECTOR; i++)
        p[lba * Block::SECTOR + i] = src[i];
    return n;
}

Block::Device* create(const char* name, uint64_t sectors,
                      const void* image, uint64_t image_bytes) {
    if (sectors == 0 || ndisks >= MAX_DISKS) return nullptr;
    if (image_bytes > sectors * Block::SECTOR) return nullptr; // does not fit

    uint64_t bytes = sectors * Block::SECTOR;
    uint8_t* p = (uint8_t*)Heap::malloc(bytes);
    if (p == nullptr) return nullptr;

    // Zero it, then lay the image over the front: the rest of the disk is
    // empty and a filesystem can tell the difference.
    for (uint64_t i = 0; i < bytes; i++) p[i] = 0;
    const uint8_t* src = (const uint8_t*)image;
    for (uint64_t i = 0; i < image_bytes; i++) p[i] = src[i];

    Block::Device* d = &devs[ndisks];
    store[ndisks] = p;
    ndisks++;

    d->name = name;
    d->sectors = sectors;
    d->read = mb_read;
    d->write = mb_write;
    d->flush = nullptr;   // a RAM disk has no cache below it
    d->priv = d; // the device is its own private data

    if (!Block::add(d)) {
        Heap::free(p);
        ndisks--;
        return nullptr;
    }
    return d;
}

} // namespace MemBlock
