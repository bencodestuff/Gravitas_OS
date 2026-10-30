#pragma once
#include <stdint.h>
#include <stddef.h>
#include <limine.h>

namespace PMM {
    const uint64_t PAGE_SIZE = 4096;

    // Initialize the manager (Read the map, build the bitmap)
    void init(struct limine_memmap_response* map);

    // Ask for a free page (Returns the physical address)
    uint64_t alloc_page();

    // Ask for a free page whose contents have been cleared. Use this for any
    // frame handed to a user address space (heap pages, user stacks): the
    // frame may still hold another task's data.
    uint64_t alloc_page_zeroed();

    // Return a page (Free it)
    void free_page(uint64_t address);

    // Mark a physical range as permanently in use (kernel infrastructure)
    void reserve_range(uint64_t base, uint64_t length);

    // Frame counters for the debugger/monitor (PAGE_SIZE frames).
    uint64_t free_frames();
    uint64_t total_frames();
}