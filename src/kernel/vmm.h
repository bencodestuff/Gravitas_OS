#pragma once
#include <stdint.h>
#include <limine.h> // Need this for the struct definition

namespace VMM {
    // UPDATED: Now requires the memory map to know what to map!
    void init(struct limine_memmap_response* map);

    // Map into the CURRENT address space
    void map_page(uint64_t physical_addr, uint64_t virtual_addr, uint64_t flags);
    // Map into an explicit address space (used for building new processes)
    void map_page_in(uint64_t pml4_phys, uint64_t physical_addr,
                     uint64_t virtual_addr, uint64_t flags);
    void unmap_page(uint64_t virtual_addr);
    void switch_pml4(uint64_t pml4_phys_addr);
    uint64_t current_pml4();          // read CR3
    uint64_t get_kernel_pml4_phys();
    bool is_mapped(uint64_t virtual_addr);

    // Create a fresh address space sharing the kernel's upper-half mappings.
    // Returns the physical address of the new PML4.
    uint64_t clone_kernel_space();

    // Free all user-half frames and tables of an address space (the kernel
    // upper half is shared and left alone). Used at process exit.
    void destroy_user_space(uint64_t pml4_phys);
}