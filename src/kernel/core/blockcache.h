#pragma once
#include <stdint.h>

#include "kernel/core/block.h"

// blockcache — a page cache in front of the block layer.
//
// Block is deliberately cacheless: it moves sectors and nothing else. A 1 KB
// ext2 block therefore used to cost one device command, even though the drive
// would happily hand over eight sectors in one go. This layer sits between the
// filesystem and Block, caches whole 4 KB pages keyed by (device, page index),
// and turns a block read into one page read whose neighbours are then hits.
//
// Pages are written back rather than through, so a filesystem that mutates a
// block several times pays for one device write. The catch is that the cache is
// then the only current view of the disk: every access to a device must come
// through here, and a sync must write dirty pages out before the drive's own
// write cache is flushed.
//
// The budget is fixed at 64 pages (256 KiB) in BSS. That is on purpose: the
// cache has to work at mount time, before the heap is a sensible place to ask,
// and a fixed table makes the eviction policy easy to reason about and test.

namespace BlockCache {

constexpr uint64_t PAGE_SIZE = 4096;
constexpr uint32_t PAGE_SECTORS = PAGE_SIZE / Block::SECTOR;  // 8
constexpr uint32_t PAGES = 64;

// Cached transfers, in sectors exactly like Block::Device::read/write.
// An all-or-nothing range check: a request past the device end fails.
bool read(Block::Device* dev, uint64_t lba, uint64_t count, void* buf);
bool write(Block::Device* dev, uint64_t lba, uint64_t count, const void* buf);

// Write back this device's dirty pages, then Block::flush(dev).
bool flush(Block::Device* dev);

struct Stats {
    uint64_t hits;
    uint64_t misses;
    uint64_t writebacks;  // dirty pages written out
    uint64_t evictions;   // pages reused
    uint64_t flushes;     // flush() calls
};
Stats stats();
void reset_stats();

} // namespace BlockCache
