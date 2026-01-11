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

    // Return a page (Free it)
    void free_page(uint64_t address);
}