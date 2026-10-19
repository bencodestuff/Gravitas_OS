#include "kernel/core/memmap.h"
#include "kernel/core/vmm.h"
#include "kernel/core/pmm.h"
#include "kernel/core/heap.h"
#include "kernel/core/sched.h"
#include "kernel/klib/klib.h"   // memset
#include "fs/vfs.h"

// See memmap.h. The registry is small and fixed: a handful of regions and
// mappings is all this system has users for, and a cap keeps a runaway client
// from eating the kernel's heap.

namespace MemMap {

constexpr int MAX_MAP    = 32;
constexpr int MAX_REGION = 16;
constexpr int MAX_GRANT  = 8;
constexpr uint64_t MAX_REGION_BYTES = 64ull << 20;

struct Map {
    bool     used;
    int      task;      // the task whose address space it is in
    uint64_t va;        // page-aligned base (what unmap() clears)
    uint64_t pages;
    int      region;    // -1 for a file mapping (nothing to release)
};

struct Region {
    bool     used;
    int      id;
    int      owner;
    int      refs;      // the creator's handle + one per mapping
    uint64_t size;      // as asked for
    uint64_t pages;
    uint64_t* frames;
    int      granted[MAX_GRANT];
    int      ngranted;
};

static Map    maps[MAX_MAP];
static Region regions[MAX_REGION];
static int    next_region_id = 1;

static uint64_t page_up(uint64_t n) { return (n + 4095) & ~(uint64_t)4095; }

// The mapping an address falls in. A file mapping hands back the file's real
// first byte, which is part-way into its first page, so the lookup cannot just
// compare bases.
static Map* map_at(uint64_t addr) {
    for (int i = 0; i < MAX_MAP; i++) {
        if (!maps[i].used) continue;
        if (addr >= maps[i].va && addr < maps[i].va + maps[i].pages * 4096)
            return &maps[i];
    }
    return nullptr;
}

static Map* map_slot() {
    for (int i = 0; i < MAX_MAP; i++)
        if (!maps[i].used) return &maps[i];
    return nullptr;
}

static Region* region_by_id(int id) {
    for (int i = 0; i < MAX_REGION; i++)
        if (regions[i].used && regions[i].id == id) return &regions[i];
    return nullptr;
}

static void region_free(Region* r) {
    for (uint64_t p = 0; p < r->pages; p++) PMM::free_page(r->frames[p]);
    Heap::free(r->frames);
    r->used = false;
    r->frames = nullptr;
}

static void region_unref(Region* r) {
    if (--r->refs <= 0) region_free(r);
}

// Map whole pages of physical memory into a task's space. Read-only unless
// 'writable', and always PTE_SHARED: the frames are not the task's to free.
static void map_pages(uint64_t cr3, uint64_t va, uint64_t phys, uint64_t pages,
                      bool writable) {
    uint64_t flags = VMM::PTE_PRESENT | VMM::PTE_USER | VMM::PTE_SHARED;
    if (writable) flags |= VMM::PTE_WRITABLE;
    for (uint64_t p = 0; p < pages; p++)
        VMM::map_page_in(cr3, phys + p * 4096, va + p * 4096, flags);
}

// --- shared regions ---

int create(uint64_t size) {
    if (size == 0) return -1;
    uint64_t span = page_up(size);
    if (span > MAX_REGION_BYTES) return -1;
    uint64_t pages = span / 4096;

    Region* r = nullptr;
    for (int i = 0; i < MAX_REGION; i++)
        if (!regions[i].used) { r = &regions[i]; break; }
    if (r == nullptr) return -1;

    uint64_t* frames = (uint64_t*)Heap::malloc(pages * sizeof(uint64_t));
    if (frames == nullptr) return -1;

    uint64_t got = 0;
    for (; got < pages; got++) {
        frames[got] = PMM::alloc_page();
        if (frames[got] == 0) break;
    }
    if (got < pages) {
        for (uint64_t p = 0; p < got; p++) PMM::free_page(frames[p]);
        Heap::free(frames);
        return -1;
    }

    // Zero it: a shared buffer must never hand one task another's bytes.
    for (uint64_t p = 0; p < pages; p++)
        memset(VMM::phys_to_kernel(frames[p]), 0, 4096);

    r->used = true;
    r->id = next_region_id++;
    r->owner = Sched::current_task_id();
    r->refs = 1;             // the creator's handle
    r->size = size;
    r->pages = pages;
    r->frames = frames;
    r->ngranted = 0;
    return r->id;
}

int grant(int id, int task) {
    Region* r = region_by_id(id);
    if (r == nullptr || r->owner != Sched::current_task_id()) return -1;
    if (task == r->owner) return 0; // it can already attach
    for (int i = 0; i < r->ngranted; i++)
        if (r->granted[i] == task) return 0; // already allowed
    if (r->ngranted >= MAX_GRANT) return -1;
    r->granted[r->ngranted++] = task;
    return 0;
}

int attach(int id, uint64_t* out_addr, uint64_t* out_size) {
    Region* r = region_by_id(id);
    if (r == nullptr) return -1;

    int me = Sched::current_task_id();
    bool allowed = (r->owner == me);
    for (int i = 0; i < r->ngranted && !allowed; i++)
        if (r->granted[i] == me) allowed = true;
    if (!allowed) return -1;

    Map* m = map_slot();
    if (m == nullptr) return -1;

    uint64_t cr3 = Sched::current_task_cr3();
    uint64_t va = VMM::alloc_user_range(cr3, r->pages);
    if (va == 0) return -1;

    // The frames came from alloc_page one at a time, so they are a scatter
    // list: map each one at its own offset rather than assuming frames[0] is
    // followed by its neighbours in physical memory.
    uint64_t flags = VMM::PTE_PRESENT | VMM::PTE_USER | VMM::PTE_SHARED |
                     VMM::PTE_WRITABLE;
    for (uint64_t p = 0; p < r->pages; p++)
        VMM::map_page_in(cr3, r->frames[p], va + p * 4096, flags);
    r->refs++;

    m->used = true;
    m->task = me;
    m->va = va;
    m->pages = r->pages;
    m->region = r->id;

    *out_addr = va;
    *out_size = r->size;
    return 0;
}

int close(int id) {
    Region* r = region_by_id(id);
    if (r == nullptr || r->owner != Sched::current_task_id()) return -1;
    region_unref(r); // mappings keep it alive if there are any
    return 0;
}

// --- file mappings ---

int map_file(const char* path, uint64_t* out_addr, uint64_t* out_size) {
    uint64_t phys = 0, size = 0;
    if (VFS::map_file(path, &phys, &size) != 0) return -1;
    if (size == 0) return -1;

    // The file starts somewhere inside its first page. The mapping covers whole
    // pages (the tail shares the image's next member — this system has no
    // secrets to leak, and the alternative is a copy) and the caller gets a
    // pointer at the file's real offset.
    uint64_t off = phys & 0xFFF;
    uint64_t base = phys & ~(uint64_t)0xFFF;
    uint64_t pages = page_up(off + size) / 4096;

    Map* m = map_slot();
    if (m == nullptr) return -1;

    uint64_t cr3 = Sched::current_task_cr3();
    uint64_t va = VMM::alloc_user_range(cr3, pages);
    if (va == 0) return -1;

    map_pages(cr3, va, base, pages, false); // the image is read-only

    m->used = true;
    m->task = Sched::current_task_id();
    m->va = va;
    m->pages = pages;
    m->region = -1;

    *out_addr = va + off;
    *out_size = size;
    return 0;
}

// --- both ---

int unmap(uint64_t addr) {
    Map* m = map_at(addr);
    if (m == nullptr || m->task != Sched::current_task_id()) return -1;

    VMM::unmap_user_range(Sched::current_task_cr3(), m->va, m->pages);
    if (m->region >= 0) {
        Region* r = region_by_id(m->region);
        if (r != nullptr) region_unref(r);
    }
    m->used = false;
    return 0;
}

void task_gone(int task_id) {
    // No unmapping: the caller is tearing the whole address space down next, and
    // PTE_SHARED means that teardown leaves the frames alone. What matters here
    // is dropping references, so a region whose owner died does not pin its
    // frames for ever.
    for (int i = 0; i < MAX_MAP; i++) {
        if (!maps[i].used || maps[i].task != task_id) continue;
        if (maps[i].region >= 0) {
            Region* r = region_by_id(maps[i].region);
            if (r != nullptr) region_unref(r);
        }
        maps[i].used = false;
    }
    for (int i = 0; i < MAX_REGION; i++)
        if (regions[i].used && regions[i].owner == task_id) region_unref(&regions[i]);
}

} // namespace MemMap
