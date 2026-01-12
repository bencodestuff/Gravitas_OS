#pragma once
#include <stdint.h>
#include <limine.h> // Need this for the struct definition

namespace VMM {
    // UPDATED: Now requires the memory map to know what to map!
    void init(struct limine_memmap_response* map);

    void map_page(uint64_t physical_addr, uint64_t virtual_addr, uint64_t flags);
    void unmap_page(uint64_t virtual_addr);
    void switch_pml4(uint64_t pml4_phys_addr);
    uint64_t get_kernel_pml4_phys();
    bool is_mapped(uint64_t virtual_addr);
}