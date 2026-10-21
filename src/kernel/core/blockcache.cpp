#include "kernel/core/blockcache.h"

#ifdef BLOCKCACHE_HOST_TEST
#include <string.h>
#else
#include "kernel/klib/klib.h"
#endif

// See blockcache.h. The table is static so the cache is usable at mount time,
// before there is a heap worth trusting, and so its whole cost is visible in
// the BSS rather than hidden behind an allocation.

namespace BlockCache {

namespace {

// A page belongs to exactly one device. Two devices never share a page number,
// even when one is the RAM disk and the other is a real drive holding a copy of
// the same image.
struct Entry {
    Block::Device* dev;
    uint64_t page;        // page index, in 4 KB units from the start of dev
    uint64_t last_used;   // LRU tick
    bool valid;
    bool dirty;
    uint8_t data[PAGE_SIZE];
};

static Entry entries[PAGES];
static uint64_t tick;
static Stats counters;

// The timer can preempt a filesystem operation halfway through a page update,
// so every public call takes this lock. Internal helpers assume it is held;
// ext2 takes the same kind of lock around its own mutations, and nesting a cli
// inside a cli is harmless because the restore only ever re-enables interrupts
// when they were on at the outermost save.
#ifdef BLOCKCACHE_HOST_TEST
static inline uint64_t irq_save() { return 0; }
static inline void irq_restore(uint64_t) {}
#else
static inline uint64_t irq_save() {
    uint64_t f;
    asm volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    if (f & (1ull << 9)) asm volatile("sti"); else asm volatile("cli");
}
#endif

// The all-or-nothing bounds rule, the same one Block enforces: a request that
// starts inside and ends outside moves nothing, rather than a short transfer.
static bool in_range(Block::Device* dev, uint64_t lba, uint64_t count) {
    return dev != nullptr && lba <= dev->sectors && count <= dev->sectors - lba;
}

static uint32_t sectors_in_page(Block::Device* dev, uint64_t page) {
    uint64_t start = page * PAGE_SECTORS;
    uint64_t n = dev->sectors - start;   // caller guarantees start < sectors
    return n < PAGE_SECTORS ? (uint32_t)n : PAGE_SECTORS;
}

// Read the page from the device in one request. A device whose size is not a
// whole number of pages has a short tail page; the bytes past the end are
// zeroed so they read as holes rather than as whatever an old page held.
static bool load_page(Block::Device* dev, uint64_t page, Entry* e) {
    uint32_t n = sectors_in_page(dev, page);
    if (dev->read(dev->priv, page * PAGE_SECTORS, n, e->data) != n) return false;
    if (n < PAGE_SECTORS)
        memset(e->data + (uint64_t)n * Block::SECTOR, 0,
               PAGE_SIZE - (uint64_t)n * Block::SECTOR);
    return true;
}

// Put a dirty page back on its device. A short tail page is written short; the
// bytes the cache zeroed past the device end are not ours to write.
static bool writeback(Entry* e) {
    uint32_t n = sectors_in_page(e->dev, e->page);
    if (e->dev->write(e->dev->priv, e->page * PAGE_SECTORS, n, e->data) != n)
        return false;
    e->dirty = false;
    counters.writebacks++;
    return true;
}

// Block::flush is the same one-line policy, but it lives with the device
// registry in block.cpp. Calling the device here keeps this module dependent on
// block.h alone, which is what lets the host test link it on its own.
static bool flush_device(Block::Device* dev) {
    return dev->flush == nullptr || dev->flush(dev->priv);
}

static Entry* lookup(Block::Device* dev, uint64_t page) {
    for (uint32_t i = 0; i < PAGES; i++) {
        Entry& e = entries[i];
        if (e.valid && e.dev == dev && e.page == page) {
            e.last_used = ++tick;
            counters.hits++;
            return &e;
        }
    }
    return nullptr;
}

// A free slot, or the least recently used one. A dirty victim goes back to the
// device first: dropping it because the budget is full would lose a write the
// caller has already been told succeeded.
static Entry* slot() {
    for (uint32_t i = 0; i < PAGES; i++)
        if (!entries[i].valid) return &entries[i];

    Entry* lru = &entries[0];
    for (uint32_t i = 1; i < PAGES; i++)
        if (entries[i].last_used < lru->last_used) lru = &entries[i];

    if (lru->dirty && !writeback(lru)) return nullptr;
    counters.evictions++;
    lru->valid = false;
    lru->dirty = false;
    return lru;
}

// Find or make the page for (dev, page). A miss is filled from the device unless
// the caller is about to overwrite the whole page, in which case reading it
// first would be a wasted command.
static Entry* touch(Block::Device* dev, uint64_t page, bool load) {
    if (Entry* e = lookup(dev, page)) return e;

    counters.misses++;
    Entry* e = slot();
    if (e == nullptr) return nullptr;

    if (load && !load_page(dev, page, e)) {
        e->valid = false;   // nothing was installed, so do not hand it out
        return nullptr;
    }

    e->dev = dev;
    e->page = page;
    e->valid = true;
    e->dirty = false;
    e->last_used = ++tick;
    return e;
}

} // namespace

bool read(Block::Device* dev, uint64_t lba, uint64_t count, void* buf) {
    if (count == 0) return true;
    if (!in_range(dev, lba, count)) return false;

    uint64_t flags = irq_save();
    uint8_t* out = (uint8_t*)buf;
    bool ok = true;

    while (count > 0) {
        uint64_t page = lba / PAGE_SECTORS;
        uint32_t off = (uint32_t)((lba % PAGE_SECTORS) * Block::SECTOR);
        uint64_t sect = PAGE_SECTORS - (lba % PAGE_SECTORS);
        if (sect > count) sect = count;

        Entry* e = touch(dev, page, true);
        if (e == nullptr) { ok = false; break; }

        memcpy(out, e->data + off, (uint64_t)sect * Block::SECTOR);
        out += (uint64_t)sect * Block::SECTOR;
        lba += sect;
        count -= sect;
    }

    irq_restore(flags);
    return ok;
}

bool write(Block::Device* dev, uint64_t lba, uint64_t count, const void* buf) {
    if (count == 0) return true;
    if (!in_range(dev, lba, count)) return false;

    uint64_t flags = irq_save();
    const uint8_t* in = (const uint8_t*)buf;
    bool ok = true;

    while (count > 0) {
        uint64_t page = lba / PAGE_SECTORS;
        uint32_t off = (uint32_t)((lba % PAGE_SECTORS) * Block::SECTOR);
        uint64_t sect = PAGE_SECTORS - (lba % PAGE_SECTORS);
        if (sect > count) sect = count;

        // A write that covers its whole page replaces every byte, so there is
        // no reason to fetch the old contents.
        bool whole = off == 0 && sect == PAGE_SECTORS;
        Entry* e = touch(dev, page, !whole);
        if (e == nullptr) { ok = false; break; }

        memcpy(e->data + off, in, (uint64_t)sect * Block::SECTOR);
        e->dirty = true;
        in += (uint64_t)sect * Block::SECTOR;
        lba += sect;
        count -= sect;
    }

    irq_restore(flags);
    return ok;
}

bool flush(Block::Device* dev) {
    if (dev == nullptr) return false;

    uint64_t flags = irq_save();
    counters.flushes++;

    bool ok = true;
    for (uint32_t i = 0; i < PAGES; i++) {
        Entry& e = entries[i];
        if (!e.valid || e.dev != dev || !e.dirty) continue;
        if (!writeback(&e)) { ok = false; break; }
    }
    // Only tell the drive to empty its own cache once the dirty pages above are
    // on the device; otherwise a power loss could drop a page we still held.
    if (ok) ok = flush_device(dev);

    irq_restore(flags);
    return ok;
}

Stats stats() {
    uint64_t flags = irq_save();
    Stats s = counters;
    irq_restore(flags);
    return s;
}

void reset_stats() {
    uint64_t flags = irq_save();
    counters = Stats{};
    irq_restore(flags);
}

} // namespace BlockCache
